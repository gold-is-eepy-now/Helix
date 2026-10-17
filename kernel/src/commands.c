/* commands.c — the built-in shell commands.
 *
 * These are the first programs that run on Helix. They are kernel-resident,
 * which is honest for this milestone: real separately-compiled user-space
 * programs need the memory manager, ELF loader and syscall ABI of later
 * milestones. The command table is deliberately trivial to extend.
 */
#include "commands.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <limine.h>

#include "console.h"
#include "cpu.h"
#include "heap.h"
#include "io.h"
#include "kstring.h"
#include "memory.h"
#include "panic.h"
#include "pit.h"
#include "pmm.h"
#include "sysinfo.h"
#include "time.h"
#include "version.h"
#include "vmm.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static bool str_eq(const char *a, const char *b) {
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static void print_u64(uint64_t value) {
    console_write_dec(value);
}
static void print_mib(uint64_t bytes) {
    print_u64(bytes / (1024u * 1024u));
    console_write(" MiB");
}

static bool parse_u64(const char *s, uint64_t *out) {
    if (s == NULL || *s == '\0') {
        return false;
    }
    uint64_t value = 0;
    for (; *s != '\0'; s++) {
        if (*s < '0' || *s > '9') {
            return false;
        }
        value = value * 10u + (uint64_t)(*s - '0');
    }
    *out = value;
    return true;
}

/* ------------------------------------------------------------------ */
/* Commands                                                            */
/* ------------------------------------------------------------------ */

static void cmd_help(int argc, char **argv) {
    (void)argc;
    (void)argv;
    console_write("Helix shell commands:\n");
    for (size_t i = 0; i < helix_command_count; i++) {
        console_set_fg(CONSOLE_BRIGHT_CYAN);
        console_write("  ");
        console_write(helix_commands[i].name);
        console_reset_color();
        /* Pad the name column for a tidy table. */
        size_t len = 0;
        for (const char *p = helix_commands[i].name; *p; p++) {
            len++;
        }
        for (size_t pad = len; pad < 10; pad++) {
            console_putc(' ');
        }
        console_write(helix_commands[i].help);
        console_putc('\n');
    }
}

static void cmd_echo(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (i > 1) {
            console_putc(' ');
        }
        console_write(argv[i]);
    }
    console_putc('\n');
}

static void cmd_clear(int argc, char **argv) {
    (void)argc;
    (void)argv;
    console_clear();
}

static void cmd_color(int argc, char **argv) {
    if (argc == 1) {
        console_write("Colour output is ");
        console_write(console_color_enabled() ? "on" : "off");
        console_write(".\n");
        console_write("usage: color on | color off\n");
        return;
    }
    if (str_eq(argv[1], "on")) {
        console_set_color_enabled(true);
        console_write("Colour output enabled.\n");
    } else if (str_eq(argv[1], "off")) {
        console_set_color_enabled(false);
        console_write("Colour output disabled; using terminal defaults.\n");
    } else {
        console_write("usage: color on | color off\n");
    }
}

static void cmd_sysinfo(int argc, char **argv) {
    (void)argc;
    (void)argv;
    sysinfo_print();
}

static void cmd_cpu(int argc, char **argv) {
    (void)argc;
    (void)argv;
    char vendor[13];
    char brand[49];
    cpu_vendor(vendor);
    cpu_brand(brand);
    console_write("Vendor: ");
    console_write(vendor);
    console_putc('\n');
    console_write("Brand:  ");
    console_write(brand);
    console_putc('\n');
    console_write("Mode:   x86_64, 64-bit long mode\n");
}

static void cmd_mem(int argc, char **argv) {
    (void)argc;
    (void)argv;
    if (!memory_available()) {
        console_set_fg(CONSOLE_YELLOW);
        console_write("No memory map from the bootloader.\n");
        console_reset_color();
        return;
    }
    console_write("Total:  ");
    print_mib(memory_total_bytes());
    console_putc('\n');
    console_write("Usable: ");
    print_mib(memory_usable_bytes());
    console_putc('\n');
    console_write("Ranges: ");
    print_u64(memory_entry_count());
    console_putc('\n');
    console_write("\nPhysical memory map:\n");
    /* memory_entry_count() is bounded in practice (tens of entries). */
    for (uint64_t i = 0; i < memory_entry_count() && i < 24; i++) {
        const struct limine_memmap_entry *e = memory_map_entry(i);
        console_write("  ");
        console_write_hex(e->base);
        console_write(" - ");
        console_write_hex(e->base + e->length);
        console_write("  ");
        print_mib(e->length);
        console_write("  ");
        console_set_fg(CONSOLE_GRAY);
        console_write(memory_type_name(e->type));
        console_reset_color();
        console_putc('\n');
    }
}

static void cmd_pmm(int argc, char **argv) {
    (void)argc;
    (void)argv;
    kprintf("Physical frame allocator\n");
    kprintf("  total     : %llu MiB (%llu frames)\n",
            (unsigned long long)(pmm_total_bytes() / (1024u * 1024u)),
            (unsigned long long)pmm_total_frames());
    kprintf("  usable    : %llu MiB (%llu frames)\n",
            (unsigned long long)(pmm_usable_frames() * PAGE_SIZE / (1024u * 1024u)),
            (unsigned long long)pmm_usable_frames());
    kprintf("  allocated : %llu frames\n",
            (unsigned long long)pmm_allocated_frames());
    kprintf("  free      : %llu MiB (%llu frames)\n",
            (unsigned long long)(pmm_free_bytes() / (1024u * 1024u)),
            (unsigned long long)pmm_free_frame_count());
    kprintf("  bitmap    : %llu bytes\n",
            (unsigned long long)pmm_bitmap_bytes());
}

static void cmd_heap(int argc, char **argv) {
    (void)argc;
    (void)argv;
    kprintf("Kernel heap\n");
    kprintf("  used   : %llu bytes\n", (unsigned long long)heap_used_bytes());
    kprintf("  total  : %llu bytes\n", (unsigned long long)heap_total_bytes());
    kprintf("  blocks : %llu\n", (unsigned long long)heap_block_count());
}

static void cmd_memtest(int argc, char **argv) {
    (void)argc;
    (void)argv;
    static const size_t sizes[16] = {
        16, 64, 256, 1024, 4096, 16384, 65536, 16,
        64, 256, 1024, 4096, 16384, 65536, 128, 512,
    };
    void *blocks[16] = {0};
    bool ok = true;

    uint64_t before = heap_used_bytes();
    kprintf("Heap self-test\n");

    for (int i = 0; i < 16; i++) {
        blocks[i] = kmalloc(sizes[i]);
        if (blocks[i] == NULL) {
            kprintf("  alloc %5llu bytes: FAILED\n", (unsigned long long)sizes[i]);
            ok = false;
            continue;
        }
        memset(blocks[i], i + 1, sizes[i]);
        kprintf("  alloc %5llu bytes: ok at %p\n",
                (unsigned long long)sizes[i], (uintptr_t)blocks[i]);
    }

    for (int i = 0; i < 16; i++) {
        if (blocks[i] == NULL) {
            continue;
        }
        const uint8_t *p = blocks[i];
        for (size_t j = 0; j < sizes[i]; j++) {
            if (p[j] != (uint8_t)(i + 1)) {
                kprintf("  pattern mismatch in block %d\n", i);
                ok = false;
                break;
            }
        }
    }

    for (int i = 0; i < 16; i++) {
        if (blocks[i] != NULL) {
            kfree(blocks[i]);
        }
    }

    uint64_t after = heap_used_bytes();
    kprintf("  heap used before: %llu bytes\n", (unsigned long long)before);
    kprintf("  heap used after:  %llu bytes\n", (unsigned long long)after);
    kprintf("  result: %s\n", (ok && before == after) ? "PASS" : "FAIL");
}

static void cmd_vmtest(int argc, char **argv) {
    (void)argc;
    (void)argv;
    const uint64_t scratch = 0xFFFF900000000000ULL;
    bool ok = true;

    kprintf("Virtual memory self-test\n");

    uint64_t phys, flags;
    if (vmm_get_mapping(scratch, &phys, &flags)) {
        kprintf("  scratch address already mapped; aborting\n");
        return;
    }

    uintptr_t frame = pmm_alloc_frame();
    if (frame == 0) {
        kprintf("  alloc frame: FAILED\n");
        return;
    }
    kprintf("  frame %p -> virtual %p\n", frame, scratch);

    if (!vmm_map(scratch, frame, VMM_PRESENT | VMM_WRITE)) {
        kprintf("  map: FAILED\n");
        pmm_free_frame(frame);
        return;
    }
    kprintf("  map: ok\n");

    volatile uint64_t *p = (volatile uint64_t *)scratch;
    p[0] = 0xDEADBEEFCAFEBABEULL;
    p[1] = 0x0123456789ABCDEFULL;
    if (p[0] != 0xDEADBEEFCAFEBABEULL || p[1] != 0x0123456789ABCDEFULL) {
        kprintf("  write/read: FAILED\n");
        ok = false;
    } else {
        kprintf("  write/read: ok\n");
    }

    if (vmm_get_mapping(scratch, &phys, &flags) && phys == frame && (flags & VMM_WRITE)) {
        kprintf("  query: ok (phys %p, flags %llx)\n", phys, (unsigned long long)flags);
    } else {
        kprintf("  query: FAILED\n");
        ok = false;
    }

    if (vmm_protect(scratch, VMM_PRESENT) &&
        vmm_get_mapping(scratch, &phys, &flags) && !(flags & VMM_WRITE)) {
        kprintf("  protect (read-only): ok\n");
    } else {
        kprintf("  protect: FAILED\n");
        ok = false;
    }

    if (vmm_unmap(scratch) && !vmm_get_mapping(scratch, &phys, &flags)) {
        kprintf("  unmap: ok\n");
    } else {
        kprintf("  unmap: FAILED\n");
        ok = false;
    }

    pmm_free_frame(frame);
    kprintf("  result: %s\n", ok ? "PASS" : "FAIL");
}

static void cmd_ticks(int argc, char **argv) {
    (void)argc;
    (void)argv;
    kprintf("PIT timer: %u Hz, %llu ticks, %llu ms\n",
            pit_hz(),
            (unsigned long long)pit_ticks(),
            (unsigned long long)pit_uptime_ms());
}

static void cmd_swint(int argc, char **argv) {
    (void)argc;
    (void)argv;
    uint64_t before = pit_ticks();
    __asm__ volatile("int $32"); /* software-trigger vector 32 (IRQ0) */
    kprintf("software int $32: ticks %llu -> %llu\n",
            (unsigned long long)before, (unsigned long long)pit_ticks());
}

static void cmd_pitcount(int argc, char **argv) {
    (void)argc;
    (void)argv;
    outb(0x43, 0x00); /* latch channel 0 */
    uint8_t l1 = inb(0x40), h1 = inb(0x40);
    for (volatile int i = 0; i < 3000000; i++) {
    }
    outb(0x43, 0x00);
    uint8_t l2 = inb(0x40), h2 = inb(0x40);
    kprintf("PIT channel 0 counter: %u -> %u\n",
            (unsigned)(l1 | (h1 << 8)), (unsigned)(l2 | (h2 << 8)));
}

static void cmd_fault(int argc, char **argv) {
    (void)argc;
    (void)argv;
    kprintf("Triggering a deliberate page fault (null dereference)...\n");
    volatile uint64_t *null = (volatile uint64_t *)0;
    *null = 1;
    kprintf("unreachable\n");
}

static void cmd_uptime(int argc, char **argv) {
    (void)argc;
    (void)argv;
    if (!time_available()) {
        console_write("Clock unavailable (TSC calibration failed).\n");
        return;
    }
    uint64_t ms = time_uptime_ms();
    console_write("Up ");
    print_u64(ms / 1000);
    console_write(".");
    print_u64((ms % 1000) / 100);
    console_write(" seconds (");
    print_u64(ms);
    console_write(" ms)\n");
}

static void cmd_version(int argc, char **argv) {
    (void)argc;
    (void)argv;
    console_write(HELIX_NAME);
    console_write(" ");
    console_write(HELIX_VERSION);
    console_write(" \"");
    console_write(HELIX_CODENAME);
    console_write("\"\n");
    console_write("Native Object Kernel, x86_64\n");
}

static void cmd_calc(int argc, char **argv) {
    if (argc != 4) {
        console_write("usage: calc <number> <+|-|*|/|%> <number>\n");
        return;
    }
    uint64_t a, b;
    if (!parse_u64(argv[1], &a) || !parse_u64(argv[3], &b)) {
        console_set_fg(CONSOLE_RED);
        console_write("calc: operands must be non-negative integers\n");
        console_reset_color();
        return;
    }
    char op = argv[2][0];
    uint64_t result;
    switch (op) {
        case '+': result = a + b; break;
        case '-':
            if (b > a) {
                console_write("calc: negative results are not supported\n");
                return;
            }
            result = a - b;
            break;
        case '*': result = a * b; break;
        case '/':
            if (b == 0) {
                console_write("calc: division by zero\n");
                return;
            }
            result = a / b;
            break;
        case '%':
            if (b == 0) {
                console_write("calc: division by zero\n");
                return;
            }
            result = a % b;
            break;
        default:
            console_write("calc: unknown operator\n");
            return;
    }
    print_u64(a);
    console_putc(' ');
    console_putc(op);
    console_putc(' ');
    print_u64(b);
    console_write(" = ");
    console_set_fg(CONSOLE_BRIGHT_GREEN);
    print_u64(result);
    console_reset_color();
    console_putc('\n');
}

static void cmd_halt(int argc, char **argv) {
    (void)argc;
    (void)argv;
    console_write("System halted. You may close QEMU.\n");
    cpu_disable_interrupts();
    for (;;) {
        cpu_halt();
    }
}

static void cmd_reboot(int argc, char **argv) {
    (void)argc;
    (void)argv;
    console_write("Rebooting...\n");
    /* Ask the PS/2 controller to pulse the CPU reset line. If that is not
     * wired up, a triple fault does the job on real hardware. */
    outb(0x64, 0xFE);
    cpu_disable_interrupts();
    for (;;) {
        cpu_halt();
    }
}

/* ------------------------------------------------------------------ */
/* The table                                                           */
/* ------------------------------------------------------------------ */

const command_t helix_commands[] = {
    {"help",    "list available commands",              cmd_help},
    {"sysinfo", "system information (Neofetch-style)",  cmd_sysinfo},
    {"neofetch","alias for sysinfo",                    cmd_sysinfo},
    {"about",   "alias for sysinfo",                    cmd_sysinfo},
    {"cpu",     "show CPU vendor and brand",            cmd_cpu},
    {"mem",     "show the physical memory map",         cmd_mem},
    {"pmm",     "physical frame allocator statistics",   cmd_pmm},
    {"heap",    "kernel heap statistics",                cmd_heap},
    {"memtest", "exercise kmalloc/kfree",                cmd_memtest},
    {"vmtest",  "exercise map/unmap/protect",            cmd_vmtest},
    {"ticks",   "show the PIT timer tick count",         cmd_ticks},
    {"swint",   "software-trigger the timer vector",      cmd_swint},
    {"pitcount","read the raw PIT counter (diagnostic)",  cmd_pitcount},
    {"fault",   "deliberately trigger a CPU exception",  cmd_fault},
    {"uptime",  "time since boot",                      cmd_uptime},
    {"echo",    "print the arguments back",             cmd_echo},
    {"calc",    "integer calculator: calc 2 + 3",       cmd_calc},
    {"clear",   "clear the screen",                     cmd_clear},
    {"color",   "turn ANSI colour on/off: color on|off", cmd_color},
    {"version", "show the OS version",                  cmd_version},
    {"halt",    "halt the machine",                     cmd_halt},
    {"reboot",  "restart the machine",                  cmd_reboot},
};

const size_t helix_command_count = sizeof(helix_commands) / sizeof(helix_commands[0]);
