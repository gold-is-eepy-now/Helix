/* main.c — the Helix kernel entry point.
 *
 * Milestone 1 proved the boot chain. Milestone 2 turns the kernel into an
 * interactive system: it brings up the console, the keyboard, a time source
 * and the memory map, prints a short boot log, and hands control to the shell.
 *
 * Objects and kernel handles are now available; scheduling, IPC, capabilities,
 * drivers, filesystems, and user space remain future milestones. See ROADMAP.md.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <limine.h>

#include "console.h"
#include "doom_wad.h"
#include "apic.h"
#include "fb.h"
#include "heap.h"
#include "idt.h"
#include "io.h"
#include "kernel_stack.h"
#include "keyboard.h"
#include "memory.h"
#include "object.h"
#include "panic.h"
#include "pic.h"
#include "pit.h"
#include "pmm.h"
#include "shell.h"
#include "sysinfo.h"
#include "thread.h"
#include "time.h"
#include "version.h"
#include "vmm.h"

/* -------------------------------------------------------------------------
 * Limine protocol requests
 *
 * Each request lives in .limine_requests (see linker.ld) so the bootloader
 * can find it, and is `volatile`/`used` so the compiler cannot discard it.
 * The bootloader writes its answer into the `response` pointer.
 * ------------------------------------------------------------------------- */

/* Base revision 6 is the newest revision this header describes. */
__attribute__((used, section(".limine_requests")))
static volatile uint64_t limine_base_revision[] = LIMINE_BASE_REVISION(6);

/* Ask for the linear framebuffer. */
__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0,
};

/* Ask for the physical memory map. */
__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0,
};

/* Ask which firmware booted us (BIOS vs UEFI), for the system-info panel. */
__attribute__((used, section(".limine_requests")))
static volatile struct limine_firmware_type_request firmware_type_request = {
    .id = LIMINE_FIRMWARE_TYPE_REQUEST_ID,
    .revision = 0,
};

/* Ask for the higher-half direct map offset: physical address P is reachable
 * at P + offset. We need this to touch physical frames the PMM hands out. */
__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0,
};

/* Optional user supplied Doom shareware/full IWAD, passed as a Limine module. */
__attribute__((used, section(".limine_requests")))
static volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST_ID,
    .revision = 0,
};

/* Limine requires these markers to bracket the request list. */
__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t limine_requests_start_marker[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t limine_requests_end_marker[] = LIMINE_REQUESTS_END_MARKER;

/* -------------------------------------------------------------------------
 * Kernel
 * ------------------------------------------------------------------------- */

/* Park the CPU forever. `cli` masks interrupts, then `hlt` sleeps the CPU. */
__attribute__((noreturn))
static void kernel_idle_loop(void) {
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

static void kernel_main_continue(void) {
    pic_remap();
    apic_allow_pic(hhdm_request.response->offset); /* let PIC IRQs through */
    pit_init(100);      /* 100 Hz system tick */
    pic_clear_mask(0);  /* unmask IRQ0 (the timer) */
    cpu_enable_interrupts();

    /* Say hello. */
    console_write("HELIX OS\n");
    console_write("Native Object Kernel\n");
    console_write("Boot successful.\n\n");
    console_write(HELIX_NAME);
    console_write(" ");
    console_write(HELIX_VERSION);
    console_write(" \"");
    console_write(HELIX_CODENAME);
    console_write("\"\n\n");

    console_set_fg(CONSOLE_GRAY);
    console_write("[helix] console: serial");
    if (fb_ready()) {
        console_write(" + framebuffer");
    }
    console_putc('\n');
    console_write("[helix] memory:  ");
    if (memory_available()) {
        console_write_dec(memory_usable_bytes() / (1024u * 1024u));
        console_write(" MiB usable\n");
    } else {
        console_write("map unavailable\n");
    }
    kprintf("[helix] pmm:     %llu MiB total, %llu MiB free (%llu frames)\n",
            (unsigned long long)(pmm_total_bytes() / (1024u * 1024u)),
            (unsigned long long)(pmm_free_bytes() / (1024u * 1024u)),
            (unsigned long long)pmm_free_frame_count());
    kprintf("[helix] heap:    %llu KiB reserved\n",
            (unsigned long long)(heap_total_bytes() / 1024u));
    console_write("[helix] vmm:     page tables active\n");
    console_write("[helix] stack:   guarded, NX, 32 KiB\n");
    kprintf("[helix] timer:   PIT %u Hz, interrupts on\n", pit_hz());
    console_write("[helix] clock:   ");
    console_write(time_available() ? "TSC calibrated\n" : "unavailable\n");
    bool doom_wad_found = false;
    if (module_request.response != NULL) {
        for (uint64_t i = 0; i < module_request.response->module_count; ++i) {
            struct limine_file *module = module_request.response->modules[i];
            if (module != NULL && module->path != NULL) {
                const char *path = module->path;
                const char *name = path;
                for (const char *p = path; *p != '\0'; ++p) {
                    if (*p == '/' || *p == '\\') name = p + 1;
                }
                if (name[0] != '\0') {
                    /* mkimage installs the optional module at this path. */
                    const char expected[] = "doom1.wad";
                    size_t n = 0;
                    while (expected[n] != '\0' && name[n] == expected[n]) ++n;
                    const unsigned char *wad = module->address;
                    bool has_iwad_header = module->size >= 12 && wad != NULL
                        && wad[0] == 'I' && wad[1] == 'W'
                        && wad[2] == 'A' && wad[3] == 'D';
                    if (expected[n] == '\0' && name[n] == '\0'
                        && has_iwad_header) {
                        doom_wad_found = true;
                        doom_wad_set(module->address, (size_t)module->size);
                    }
                }
            }
        }
    }
    console_write("[helix] doom:    ");
    console_write(doom_wad_found ? "IWAD module found (engine port in progress)\n"
                                 : "no IWAD module; set DOOM_WAD when building\n");
    console_reset_color();
    console_putc('\n');

    console_write("Type 'help' for a list of commands, 'sysinfo' for system info.\n\n");

    /* M7: boot the scheduler. The boot thread runs the kernel; a demo worker
     * is queued so the first context switch has something to switch to. */
    thread_init();
    thread_create(thread_demo_main);
    thread_yield();

    shell_run();
    kernel_idle_loop();
}

void kmain(void) {
    /* 1. Did the bootloader understand our protocol revision? */
    if (!LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision)) {
        kernel_idle_loop();
    }

    /* 2. Find a framebuffer, if the firmware offered one. Serial works
     *    regardless, so this is best-effort. */
    struct limine_framebuffer *fb = NULL;
    if (framebuffer_request.response != NULL
        && framebuffer_request.response->framebuffer_count >= 1) {
        fb = framebuffer_request.response->framebuffers[0];
    }

    /* 3. Bring up the kernel's subsystems. */
    console_init(fb);
    keyboard_init();
    memory_init(memmap_request.response);
    if (firmware_type_request.response != NULL) {
        sysinfo_set_firmware(firmware_type_request.response->firmware_type);
    }
    time_init();

    /* Physical memory, then the heap built on top of it. */
    uint64_t hhdm_offset = (hhdm_request.response != NULL)
                         ? hhdm_request.response->offset
                         : 0;
    pmm_init(memmap_request.response, hhdm_offset);
    heap_init(hhdm_offset);
    object_manager_init();

    /* Adopt page tables, install exceptions, map a protected stack, then move
     * all ordinary kernel execution onto it before enabling interrupts. */
    vmm_init(hhdm_offset);
    idt_init();
    if (!kernel_stack_init(hhdm_offset)) {
        panic("failed to establish NX kernel stack");
    }
    kernel_stack_enter(KERNEL_STACK_TOP, kernel_main_continue);
}
