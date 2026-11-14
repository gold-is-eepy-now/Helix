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
#include "linux_compat.h"
#include "usermode.h"
#include "userbin.h"
#include "doom_wad.h"
#include "icon_pack.h"
#include "filesystem.h"
#include "apic.h"
#include "fb.h"
#include "heap.h"
#include "idt.h"
#include "gdt.h"
#include "io.h"
#include "kernel_stack.h"
#include "keyboard.h"
#include "memory.h"
#include "object.h"
#include "panic.h"
#include "pic.h"
#include "pit.h"
#include "pmm.h"
#include "ramdisk_driver.h"
#include "ahci.h"
#include "driver.h"
#include "block_device.h"
#include "shell.h"
#include "sysinfo.h"
#include "thread.h"
#include "time.h"
#include "version.h"
#include "vmm.h"
#include "usb_hid.h"
#include "syscall.h"
#include "usb_host.h"
#include "acpi.h"
#include "intel_lpss_gspi.h"
#include "xhci.h"
#include "e1000.h"
#include "net.h"
#include "klog.h"
#include "ui.h"
#include "accounts.h"
#include "virtio_gpu.h"
#include "ext4.h"

/* M11: ext4 root time source (wall-clock seconds since the Unix epoch). */
static uint32_t wallclock_secs(void) {
    return (uint32_t)time_epoch();
}

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

/* Ask for the wall-clock timestamp at boot. */
__attribute__((used, section(".limine_requests")))
static volatile struct limine_date_at_boot_request date_at_boot_request = {
    .id = LIMINE_DATE_AT_BOOT_REQUEST_ID,
    .revision = 0,
};

/* Ask where the ACPI root pointer (RSDP) is, for the firmware tables (acpi.c). */
__attribute__((used, section(".limine_requests")))
static volatile struct limine_rsdp_request rsdp_request = {
    .id = LIMINE_RSDP_REQUEST_ID,
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
    console_write("\"  built ");
    console_write(HELIX_BUILD_DATE);
    console_write("\n\n");

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
    console_write("[helix] date:    ");
    console_write(time_epoch() ? "wall clock\n" : "unavailable\n");
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
                    /* The Oxygen icon pack, installed by mkimage at boot/icons/. */
                    const char icon_pack_name[] = "oxygen-32.pak";
                    size_t k = 0;
                    while (icon_pack_name[k] != '\0' && name[k] == icon_pack_name[k]) ++k;
                    if (icon_pack_name[k] == '\0' && name[k] == '\0') {
                        icon_pack_set(module->address, (size_t)module->size);
                    }
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

    /* The syscall table is filled here; int $0x80 (user code and tests) uses it. */
    syscall_init();

    /* USB needs our page tables (vmm_init ran in kmain) to map the EHCI registers,
     * so it starts here, before the shell takes the keyboard. See docs/07-usb.md. */
    /* Firmware tables first: they are read-only and do not depend on the devices. */
    if (rsdp_request.response != NULL && rsdp_request.response->address != NULL) {
        if (!acpi_init(hhdm_request.response->offset, (uint64_t)(uintptr_t)rsdp_request.response->address)) {
            console_write("[acpi] RSDP or root table failed its checksum\n");
        }
    } else {
        console_write("[acpi] no RSDP from the bootloader\n");
    }
    if (acpi_table_count() > 0) {
        kprintf("[acpi] ACPI revision %u, %u tables\n", (unsigned)acpi_revision() + 1u,
                acpi_table_count());
    }

    /* The Intel LPSS GSPI controller (MacBook Air 7,2). Optional: absent on QEMU, and the
     * system runs without it. See docs/17-spi.md. */
    bool gspi_ready = intel_lpss_gspi_init();
    intel_lpss_gspi_info_t gspi;
    intel_lpss_gspi_info(&gspi);
    if (!gspi.found) {
        kprintf("[spi] Intel LPSS GSPI 8086:9CE6 not found\n");
    } else {
        kprintf("[spi] Intel LPSS GSPI 8086:9CE6 at %02x:%02x.%u, ready %s\n", gspi.bus,
                gspi.device, gspi.function, gspi_ready ? "yes" : "no");
    }

    /* xHCI (USB 3): read-only discovery. Logged only; nothing is written to the controller. */
    if (xhci_init()) {
        xhci_info_t x;
        xhci_get_info(&x);
        kprintf("[xhci] found at %02x:%02x.%u, version 0x%04x, %u ports, %u connected\n",
                x.bus, x.device, x.function, (unsigned)x.hci_version, (unsigned)x.max_ports,
                (unsigned)x.ports_connected);
    } else {
        kprintf("[xhci] no xHCI controller found\n");
    }

    usb_report_pci_controllers();

    if (!usb_host_init(hhdm_request.response->offset)) {
        console_write("no UHCI controller found or started\n");
    }

    /* The network card (Intel e1000). Absent cards are reported by `net`, not at boot. */
    e1000_init();
    net_init();                 /* the IPv4 stack on top of the card; see docs/10-network.md */
    if (virtio_gpu_init()) {    /* the virtio-gpu display list, if QEMU provides the device */
        void *pixels = NULL;
        uint32_t w = 0, h = 0, pitch = 0;
        if (virtio_gpu_display_init() && virtio_gpu_backing(&pixels, &w, &h, &pitch) &&
            fb_use_virtio(pixels, w, h)) {
            klog_note(KLOG_DESKTOP, "[display] drawing through virtio-gpu (%ux%u)\n", w, h);
        } else {
            klog_note(KLOG_DESKTOP, "[display] staying on the firmware framebuffer\n");
        }
    }

    /* M9: install the programs into /bin, then start PID 1 (/bin/init) in ring 3.
     * init starts /bin/sh, so this runs after USB (the keyboard may be on USB) and
     * before the kernel shell takes over. When the userland shell exits, PID 1 exits
     * and the kernel shell starts. */
    if (!userbin_install()) {
        kprintf("[init] could not install /bin\n");
    }
    usermode_env_reset();   /* default PATH and HOME for every program, on every boot */
    if (!fb_ready()) {      /* graphical boots go to the sign-in screen instead */
        const char *init_argv[] = { "/bin/init", NULL };
        int64_t init_status = usermode_spawn("/bin/init", 1, init_argv);
        if (init_status < 0) {
            kprintf("[init] could not start /bin/init (status %d)\n", (int)init_status);
        } else {
            kprintf("[init] PID 1 exited with status %d\n", (int)init_status);
        }
    }

    console_write("Type 'help' for a list of commands, 'sysinfo' for system info.\n\n");

    /* M8: boot the scheduler. The boot thread runs the kernel; thread_init
     * queues the demo pair (rendezvous sender/receiver) before returning. */
    thread_init();
    thread_yield();

    ui_run();            /* sign-in, desktop and TUI shell; falls back to the serial shell */
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
    uint64_t boot_epoch = 0;
    if (date_at_boot_request.response != NULL) {
        boot_epoch = (uint64_t)date_at_boot_request.response->timestamp;
    }
    if (boot_epoch == 0) {
        boot_epoch = time_rtc_epoch();
    }
    time_set_boot_epoch(boot_epoch);

    /* Physical memory, then the heap built on top of it. */
    uint64_t hhdm_offset = (hhdm_request.response != NULL)
                         ? hhdm_request.response->offset
                         : 0;
    pmm_init(memmap_request.response, hhdm_offset);
    heap_init(hhdm_offset);
    object_manager_init();

    /* M10: initialize driver framework and drivers. */
    ramdisk_driver_init();
    driver_init_all();
    block_device_init_all();

    /* LC-1: Linux compatibility layer self-test and any compat initcalls. */
    compat_run_initcalls();

    /* M11: mount ext4 root from ramdisk block device as root filesystem. */
    ext4_set_time_source(wallclock_secs);
    block_device_t *rbd = block_device_ramdisk();
    if (rbd == NULL) {
        console_write("[main] ramdisk device is NULL\n");
    } else if (rbd->block_size == 0 || rbd->block_count == 0) {
        console_write("[main] ramdisk not initialized: block_size=");
        console_write_dec(rbd->block_size);
        console_write(", block_count=");
        console_write_dec(rbd->block_count);
        console_putc('\n');
    }
    ext4_fs_t *ext4fs = ext4_mount(rbd, false);
    if (ext4fs == NULL) {
        /* Ramdisk is empty - format it with ext4 like a live Linux distro */
        console_write("[main] formatting ramdisk with ext4...\n");
        if (ext4_format(rbd)) {
            ext4fs = ext4_mount(rbd, false);
        }
    }
    if (ext4fs == NULL) {
        console_write("[main] ext4 mount failed after format, falling back to helixfs\n");
        /* Fall back to the original helix boot filesystem */
        fs_boot_init();
        fs_mount(NULL, "/", "helixfs", true);
        fs_boot_add_file("README", "Helix OS\nboot filesystem active.\n", 0);
        fs_boot_add_file("LICENSE", "MIT License\n\nCopyright (c) 2025 Helix OS\n\nPermission is hereby granted, free of charge, ...\n", 0);
    } else {
        console_write("[main] mounted ext4 from ramdisk as root\n");
        accounts_init();
        /* ext4_set_time_source and ext4_mount already set g_ext4_current */
    }

    /* Adopt page tables, install exceptions, map a protected stack, then move
     * all ordinary kernel execution onto it before enabling interrupts. */
    vmm_init(hhdm_offset);

    /* AHCI (SATA) maps its host registers with vmm_map, so it must run after
     * vmm_init has set the page-table pointer. Registers sataN disks; read-only. */
    if (ahci_init()) kprintf("[ahci] %d SATA disk(s) registered\n", ahci_disk_count());
    else kprintf("[ahci] no AHCI controller found\n");

    gdt_init();

    idt_init();
    if (!kernel_stack_init(hhdm_offset)) {
        panic("failed to establish NX kernel stack");
    }
    kernel_stack_enter(KERNEL_STACK_TOP, kernel_main_continue);
}
