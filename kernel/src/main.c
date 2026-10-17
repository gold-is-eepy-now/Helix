/* main.c — the Helix kernel entry point.
 *
 * Milestone 1 proved the boot chain. Milestone 2 turns the kernel into an
 * interactive system: it brings up the console, the keyboard, a time source
 * and the memory map, prints a short boot log, and hands control to the shell.
 *
 * Still absent, by design: memory allocation, scheduling, IPC, objects,
 * capabilities, drivers, filesystems, user space. See ROADMAP.md.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <limine.h>

#include "console.h"
#include "apic.h"
#include "fb.h"
#include "heap.h"
#include "idt.h"
#include "io.h"
#include "keyboard.h"
#include "memory.h"
#include "panic.h"
#include "pic.h"
#include "pit.h"
#include "pmm.h"
#include "shell.h"
#include "sysinfo.h"
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
    pmm_init(memmap_request.response);
    uint64_t hhdm_offset = (hhdm_request.response != NULL)
                         ? hhdm_request.response->offset
                         : 0;
    heap_init(hhdm_offset);

    /* Virtual memory (our own page tables on top of Limine's) and interrupts. */
    vmm_init(hhdm_offset);
    idt_init();
    pic_remap();
    apic_allow_pic(hhdm_offset); /* let PIC IRQs through if the LAPIC is on */
    pit_init(100);      /* 100 Hz system tick */
    pic_clear_mask(0);  /* unmask IRQ0 (the timer) */
    cpu_enable_interrupts();

    /* 4. Say hello. */
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
    kprintf("[helix] timer:   PIT %u Hz, interrupts on\n", pit_hz());
    console_write("[helix] clock:   ");
    console_write(time_available() ? "TSC calibrated\n" : "unavailable\n");
    console_reset_color();
    console_putc('\n');

    console_write("Type 'help' for a list of commands, 'sysinfo' for system info.\n\n");

    /* 5. Become an interactive system. Never returns. */
    shell_run();
    kernel_idle_loop();
}
