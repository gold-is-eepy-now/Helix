/* main.c — the Helix kernel entry point.
 *
 * Milestone 1 proved the boot chain. Milestone 2 turns the kernel into an
 * interactive system: it brings up the console, the keyboard, a time source
 * and the memory map, prints a short boot log, and hands control to the shell.
 *
 * Objects, capabilities, and the first cooperative kernel threads are now
 * available. Single-CPU preemption and the first bounded IPC channels are
 * implemented; blocking IPC, drivers, filesystems, and user space remain
 * future milestones. See ROADMAP.md.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <limine.h>

#include "console.h"
#include "ahci.h"
#include "boot_app.h"
#include "commands.h"
#include "doom_wad.h"
#include "driver.h"
#include "ehci.h"
#include "apic.h"
#include "fb.h"
#include "gdt.h"
#include "heap.h"
#include "install.h"
#include "idt.h"
#include "io.h"
#include "kernel_stack.h"
#include "keyboard.h"
#include "kstring.h"
#include "memory.h"
#include "object.h"
#include "panic.h"
#include "pci.h"
#include "pic.h"
#include "pit.h"
#include "pmm.h"
#include "shell.h"
#include "serial.h"
#include "sysinfo.h"
#include "thread.h"
#include "time.h"
#include "version.h"
#include "vmm.h"
#include "xhci.h"
#include "usermode.h"

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

/* These devices are initialized by their existing boot paths. The first
 * registry lifecycle callback confirms ownership has been established. */
static bool core_driver_start(void *context) { return context != NULL; }
static bool uart_driver_control(void *context, uint32_t request,
                                uint64_t argument) {
    (void)context;
    if (request != DRIVER_CONTROL_UART_WRITE_BYTE || argument > 0xffu ||
        argument == '\n' || argument == '\r') return false;
    return serial_try_putc((char)argument);
}
static const char g_uart_driver_name[] = "uart16550";
static const char g_keyboard_driver_name[] = "i8042-keyboard";
static const char g_pit_driver_name[] = "pit";
static const char g_xhci_driver_name[] = "xhci-usb";
static const char g_ehci_driver_name[] = "ehci-usb";
static const driver_descriptor_t g_uart_driver = {
    .name = g_uart_driver_name, .device_class = "serial-console",
    .context = (void *)g_uart_driver_name, .start = core_driver_start,
    .control = uart_driver_control,
};
static const driver_descriptor_t g_keyboard_driver = {
    .name = g_keyboard_driver_name, .device_class = "keyboard",
    .context = (void *)g_keyboard_driver_name, .start = core_driver_start,
};
static const driver_descriptor_t g_pit_driver = {
    .name = g_pit_driver_name, .device_class = "timer",
    .context = (void *)g_pit_driver_name, .start = core_driver_start,
};
static bool xhci_driver_start(void *context) {
    return context != NULL && xhci_ready();
}
static const driver_descriptor_t g_xhci_driver = {
    .name = g_xhci_driver_name, .device_class = "usb-host-controller",
    .context = (void *)g_xhci_driver_name, .start = xhci_driver_start,
};
static bool ehci_driver_start(void *context) {
    return context != NULL && ehci_ready();
}
static const driver_descriptor_t g_ehci_driver = {
    .name = g_ehci_driver_name, .device_class = "usb-host-controller",
    .context = (void *)g_ehci_driver_name, .start = ehci_driver_start,
};
static bool usb_storage_driver_start(void *context) {
    return context != NULL && (xhci_storage_ready() || ehci_storage_ready());
}
static bool usb_storage_driver_read(void *context, uint64_t lba,
                                    void *buffer, size_t length) {
    if (context == NULL) return false;
    return xhci_storage_ready() ? xhci_read_sector(lba, buffer, length)
                                : ehci_read_sector(lba, buffer, length);
}
static bool usb_storage_driver_write(void *context, uint64_t lba,
                                     const void *buffer, size_t length) {
    if (context == NULL) return false;
    return xhci_storage_ready() ? xhci_write_sector(lba, buffer, length)
                                : ehci_write_sector(lba, buffer, length);
}
static bool usb_storage_driver_flush(void *context) {
    if (context == NULL) return false;
    return xhci_storage_ready() ? xhci_flush() : ehci_flush();
}
static const char g_usb_storage_name[] = "usb-msd";
static driver_descriptor_t g_usb_storage_driver = {
    .name = g_usb_storage_name, .device_class = "block-storage",
    .context = (void *)g_usb_storage_name, .start = usb_storage_driver_start,
    .read = usb_storage_driver_read, .write = usb_storage_driver_write,
    .flush = usb_storage_driver_flush,
};

bool usb_storage_register_if_ready(void) {
    if (!xhci_storage_ready() && !ehci_storage_ready()) return false;
    for (size_t i = 0; i < driver_count(); i++) {
        driver_info_t info;
        if (driver_get_info(i, &info) && strcmp(info.name, "usb-msd") == 0)
            return true;
    }
    g_usb_storage_driver.sector_size = xhci_storage_ready()
        ? xhci_sector_size() : ehci_sector_size();
    g_usb_storage_driver.sector_count = xhci_storage_ready()
        ? xhci_sector_count() : ehci_sector_count();
    return driver_register(&g_usb_storage_driver);
}

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
    console_write("[helix] kernel core ready; initializing devices\n");
    pic_remap();
    apic_allow_pic(hhdm_request.response->offset); /* let PIC IRQs through */
    pit_init(100);      /* 100 Hz system tick */
    if (!driver_register(&g_pit_driver))
        panic("failed to register PIT driver");
    console_write("[helix] pci: scanning controllers\n");
    pci_init(hhdm_request.response->offset);
    console_write("[helix] pci: scan complete\n");
    console_write("[helix] xhci: initializing\n");
    if (xhci_init(hhdm_request.response->offset)) {
        if (!driver_register(&g_xhci_driver))
            console_write("[helix] xhci: started; registry record failed\n");
    } else {
        console_write("[helix] ehci: initializing USB 2.0 controller\n");
        if (ehci_init(hhdm_request.response->offset)) {
            if (!driver_register(&g_ehci_driver))
                console_write("[helix] ehci: started; registry record failed\n");
        } else if (pci_usb_controller_count() != 0) {
            console_write("[helix] usb: supported host controller start failed\n");
        }
    }
    if ((xhci_storage_ready() || ehci_storage_ready()) &&
        !usb_storage_register_if_ready())
        console_write("[helix] usb: storage registry record failed\n");
    console_write("[helix] ahci: initializing SATA\n");
    (void)ahci_init(hhdm_request.response->offset);
    console_write("[helix] storage: controller initialization complete\n");
    pic_clear_mask(0);  /* unmask IRQ0 (the timer) */
    if (!thread_system_init(hhdm_request.response->offset)) {
        panic("failed to initialize kernel thread subsystem");
    }
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
    console_write("[helix] console: ");
    console_write(serial_available() ? "serial" : "serial unavailable");
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
    bool installer_image_found = false;
    size_t helix_app_count = 0;
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
                    size_t name_len = 0;
                    while (name[name_len] != '\0') name_len++;
                    if (name_len >= 4 &&
                        name[name_len - 4] == '.' &&
                        name[name_len - 3] == 'h' &&
                        name[name_len - 2] == 'l' &&
                        name[name_len - 1] == 'x' &&
                        module->address != NULL && module->size != 0) {
                        if (boot_app_set(name, module->address,
                                         (size_t)module->size))
                            helix_app_count++;
                    }
                    static const char installer_image_name[] = "helix-install.img";
                    size_t installer_name_len = sizeof(installer_image_name) - 1;
                    if (!installer_image_found &&
                        name_len == installer_name_len &&
                        memcmp(name, installer_image_name, installer_name_len) == 0 &&
                        module->size <= SIZE_MAX &&
                        install_image_set(module->address,
                                          (size_t)module->size))
                        installer_image_found = true;
                }
            }
        }
    }
    console_write("[helix] doom:    ");
    console_write(doom_wad_found ? "IWAD module found (engine port in progress)\n"
                                 : "no IWAD module; set DOOM_WAD when building\n");
    console_write("[helix] app:     ");
    if (helix_app_count == 0) console_write("no .hlx boot module\n");
    else { console_write_dec(helix_app_count); console_write(" .hlx boot module(s) found\n"); }
    console_write("[helix] installer: ");
    console_write(installer_image_found ? "disk image ready\n" : "no install image module\n");
    console_reset_color();
    console_putc('\n');

    if (commands_start_init()) {
        console_write("\nInitial user process returned; entering maintenance shell.\n");
    } else {
        console_write("No user init app; entering kernel maintenance shell.\n");
    }
    console_write("Type 'help' for a list of commands, 'sysinfo' for system info.\n\n");
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
    console_write("[helix] kernel entry; console initialized\n");
    keyboard_init();
    console_write("[helix] keyboard initialized\n");
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
    console_write("[helix] physical memory initialized\n");
    heap_init(hhdm_offset);
    object_manager_init();
    driver_manager_init();
    if (!driver_register(&g_uart_driver) ||
        !driver_register(&g_keyboard_driver))
        panic("failed to register core device drivers");

    /* Adopt page tables, install exceptions, map a protected stack, then move
     * all ordinary kernel execution onto it before enabling interrupts. */
    vmm_init(hhdm_offset);
    console_write("[helix] virtual memory initialized\n");
    if (!kernel_stack_init(hhdm_offset)) {
        panic("failed to establish NX kernel stack");
    }
    if (!gdt_init(KERNEL_STACK_TOP)) {
        panic("failed to establish GDT and TSS");
    }
    if (!usermode_syscall_init()) {
        panic("CPU does not support native syscall entry");
    }
    idt_init();
    kernel_stack_enter(KERNEL_STACK_TOP, kernel_main_continue);
}
