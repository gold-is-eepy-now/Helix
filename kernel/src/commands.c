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

#include "capability.h"
#include "console.h"
#include "cpu.h"
#include "heap.h"
#include "io.h"
#include "kernel_stack.h"
#include "kstring.h"
#include "memory.h"
#include "object.h"
#include "panic.h"
#include "pit.h"
#include "pmm.h"
#include "sysinfo.h"
#include "time.h"
#include "version.h"
#include "vmm.h"
#include "filesystem.h"
#include "display_surface.h"
#include "input_device.h"
#include "net_interface.h"
#include "ext4.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* Forward declarations for recursive tree helpers (defined further down). */
static bool cp_copy_dir(ext4_fs_t *fs, uint64_t src_ino, uint64_t dst_parent,
                        const char *dst_name);
static bool rm_remove_dir(ext4_fs_t *fs, uint64_t dir_ino, uint64_t parent_ino,
                          const char *name);

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

static void cmd_neofetch(int argc, char **argv) {
    (void)argc;
    (void)argv;
    sysinfo_print();
}

static void cmd_about(int argc, char **argv) {
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

static void cmd_stacktest(int argc, char **argv) {
    (void)argc;
    (void)argv;
    console_write("Kernel stack protection self-test\n");
    bool active = kernel_stack_active();
    console_write(active ? "  execution is on the dedicated stack: ok\n"
                          : "  execution is on the dedicated stack: FAILED\n");

    uint64_t guard_error = UINT64_MAX;
    bool guard_caught = kernel_stack_probe_guard(&guard_error);
    bool guard_ok = guard_caught && (guard_error & 0x1fu) == 0;
    kprintf("  guard page fault: %s (error %p)\n",
            guard_ok ? "reported and recovered" : "FAILED",
            (uintptr_t)guard_error);

    uint64_t nx_error = UINT64_MAX;
    bool nx_caught = kernel_stack_probe_nx(&nx_error);
    bool nx_ok = nx_caught && (nx_error & 0x11u) == 0x11u &&
                  (nx_error & 0x0eu) == 0;
    kprintf("  NX execute fault: %s (error %p)\n",
            nx_ok ? "reported and recovered" : "FAILED",
            (uintptr_t)nx_error);

    bool ok = active && guard_ok && nx_ok;
    console_write(ok ? "  M3 stack self-test: PASS\n"
                     : "  M3 stack self-test: FAIL\n");
}

/* Fixed object test implementations that need to be in the file */
static uint64_t g_objtest_destroyed;

static void objtest_destroy(object_t *object, void *payload) {
    (void)object;
    (void)payload;
    g_objtest_destroyed++;
}

static bool objtest_check(bool passed, const char *label) {
    console_write("  ");
    console_write(label);
    console_write(passed ? ": ok\n" : ": FAILED\n");
    return passed;
}

static void cmd_objects(int argc, char **argv) {
    (void)argc;
    (void)argv;
    kprintf("Object manager\n");
    kprintf("  live objects : %llu\n", (unsigned long long)object_count());
    kprintf("  kernel handles: %llu / %u\n",
            (unsigned long long)handle_count(object_kernel_handles()),
            HANDLE_TABLE_CAPACITY);
    console_write("  synchronization: single-CPU, kernel-context callers only\n");
}

static void cmd_objtest(int argc, char **argv) {
    (void)argc;
    (void)argv;
    console_write("Object and handle self-test\n");

    const uint64_t before = object_count();
    g_objtest_destroyed = 0;
    handle_table_t table;
    handle_table_init(&table);
    object_t *event = object_create(OBJECT_TYPE_EVENT, sizeof(uint64_t), objtest_destroy);
    object_t *channel = object_create(OBJECT_TYPE_CHANNEL, sizeof(uint64_t), objtest_destroy);
    bool ok = objtest_check(event != NULL && channel != NULL &&
                            object_count() == before + 2,
                            "create event and channel objects");
    if (event == NULL || channel == NULL) {
        if (event != NULL) object_release(event);
        if (channel != NULL) object_release(channel);
        objtest_check(false, "remaining lifetime checks (allocation failed)");
        return;
    }

    uint64_t event_id = object_id(event);
    uint64_t channel_id = object_id(channel);
    handle_t event_handle = handle_create(&table, event,
        OBJECT_RIGHT_READ | OBJECT_RIGHT_WRITE | OBJECT_RIGHT_DUPLICATE |
        OBJECT_RIGHT_INSPECT);
    handle_t channel_handle = handle_create(&table, channel,
        OBJECT_RIGHT_READ | OBJECT_RIGHT_INSPECT);
    object_release(event);
    object_release(channel);
    ok &= objtest_check(event_handle != 0 && channel_handle != 0 &&
                        handle_count(&table) == 2,
                        "create handles with per-handle rights");

    object_t *looked_up = object_lookup(channel_id);
    bool lookup_ok = looked_up != NULL && object_type(looked_up) == OBJECT_TYPE_CHANNEL;
    if (looked_up != NULL) object_release(looked_up);
    ok &= objtest_check(lookup_ok, "global lookup retains the requested type");

    object_t *got = handle_get(&table, event_handle, OBJECT_RIGHT_READ,
                               OBJECT_TYPE_EVENT);
    bool get_ok = got != NULL && object_type(got) == OBJECT_TYPE_EVENT;
    if (got != NULL) object_release(got);
    ok &= objtest_check(get_ok, "handle lookup checks type and rights");
    ok &= objtest_check(handle_get(&table, channel_handle, OBJECT_RIGHT_WRITE,
                                   OBJECT_TYPE_NONE) == NULL &&
                        handle_get(&table, channel_handle, OBJECT_RIGHT_READ,
                                   OBJECT_TYPE_EVENT) == NULL &&
                        handle_get(&table, event_handle, 0,
                                   OBJECT_TYPE_NONE) == NULL &&
                        handle_get(&table, 0x7fffffffu, OBJECT_RIGHT_READ,
                                   OBJECT_TYPE_NONE) == NULL,
                        "reject missing/zero rights, wrong type, and invalid handle");

    handle_info_t info;
    ok &= objtest_check(handle_inspect(&table, event_handle, &info) &&
                        info.object_id == event_id &&
                        info.type == OBJECT_TYPE_EVENT &&
                        (info.rights & OBJECT_RIGHT_DUPLICATE),
                        "inspect handle metadata");

    handle_t reduced = handle_duplicate(&table, event_handle,
        OBJECT_RIGHT_READ | OBJECT_RIGHT_INSPECT);
    ok &= objtest_check(reduced != 0 &&
                        handle_duplicate(&table, event_handle,
                                         OBJECT_RIGHT_CONTROL) == 0 &&
                        handle_duplicate(&table, channel_handle,
                                         OBJECT_RIGHT_READ) == 0,
                        "duplicate only with reduced rights");

    bool closed_source = handle_close(&table, event_handle);
    bool stale_rejected = !handle_close(&table, event_handle) &&
        handle_get(&table, event_handle, OBJECT_RIGHT_READ, OBJECT_TYPE_NONE) == NULL;
    object_t *temporary = object_lookup(event_id);
    bool alive_through_duplicate = temporary != NULL;
    if (temporary != NULL) object_release(temporary);
    ok &= objtest_check(closed_source && stale_rejected && alive_through_duplicate,
                        "close invalidates a handle but preserves shared object");

    bool closed_duplicate = handle_close(&table, reduced);
    bool destroyed_once = g_objtest_destroyed == 1 && object_lookup(event_id) == NULL;
    ok &= objtest_check(closed_duplicate && destroyed_once,
                        "destroy object after its final handle closes");
    bool closed_channel = handle_close(&table, channel_handle);
    bool invalid_close_rejected = !handle_close(&table, channel_handle);
    ok &= objtest_check(closed_channel && invalid_close_rejected &&
                        object_lookup(channel_id) == NULL &&
                        g_objtest_destroyed == 2 && object_count() == before,
                        "close channel once and release final reference");
    handle_table_close_all(&table);
    console_write(ok ? "  M5 object self-test: PASS\n"
                     : "  M5 object self-test: FAIL\n");
}

static void cmd_captest(int argc, char **argv) {
    (void)argc;
    (void)argv;
    console_write("Capability self-test\n");

    uint64_t before = object_count();
    g_objtest_destroyed = 0;
    handle_table_t table;
    handle_table_init(&table);
    object_t *target = object_create(OBJECT_TYPE_EVENT, 0, objtest_destroy);
    if (target == NULL) {
        console_write("  create target object: FAILED\n");
        console_write("  M6 capability self-test: FAIL\n");
        return;
    }
    handle_t source = handle_create(&table, target,
        OBJECT_RIGHT_READ | OBJECT_RIGHT_WRITE | OBJECT_RIGHT_DUPLICATE |
        OBJECT_RIGHT_INSPECT);
    object_release(target);
    bool ok = objtest_check(source != 0, "create source object handle");

    handle_t cap = capability_create(&table, source,
                                     OBJECT_RIGHT_READ | OBJECT_RIGHT_WRITE);
    handle_t child = capability_derive(&table, cap, OBJECT_RIGHT_READ);
    bool overgrant_rejected = capability_derive(&table, child,
                                                OBJECT_RIGHT_WRITE) == 0 &&
        capability_create(&table, source, OBJECT_RIGHT_CONTROL) == 0;
    ok &= objtest_check(cap != 0 && child != 0,
                        "create capability and derive reduced rights");
    ok &= objtest_check(overgrant_rejected,
                        "reject capability rights enlargement");
    ok &= objtest_check(capability_create(&table, source, 0) == 0,
                        "reject capability creation with zero rights");

    bool allows_write = capability_allows(&table, cap, OBJECT_RIGHT_WRITE,
                                          OBJECT_TYPE_EVENT);
    bool allows_insufficient = !capability_allows(&table, child,
                                                   OBJECT_RIGHT_WRITE, OBJECT_TYPE_NONE);
    ok &= objtest_check(allows_write && allows_insufficient,
                        "capability_allows checks granted rights and target type");

    object_t *resolved = capability_resolve(&table, cap, OBJECT_RIGHT_WRITE,
                                           OBJECT_TYPE_EVENT);
    bool resolve_ok = resolved != NULL;
    if (resolved != NULL) object_release(resolved);
    object_t *forbidden = capability_resolve(&table, child, OBJECT_RIGHT_WRITE,
                                             OBJECT_TYPE_NONE);
    object_t *wrong_type = capability_resolve(&table, cap, OBJECT_RIGHT_READ,
                                              OBJECT_TYPE_CHANNEL);
    ok &= objtest_check(resolve_ok && forbidden == NULL && wrong_type == NULL,
                        "enforce granted rights and target type");
    if (forbidden != NULL) object_release(forbidden);
    if (wrong_type != NULL) object_release(wrong_type);
    ok &= objtest_check(!capability_revoke(&table, child),
                        "derived capability cannot revoke its parent");

    bool revoked = capability_revoke(&table, cap);
    ok &= objtest_check(revoked && capability_is_revoked(&table, child) &&
                        capability_resolve(&table, cap, OBJECT_RIGHT_READ,
                                           OBJECT_TYPE_NONE) == NULL &&
                        capability_resolve(&table, child, OBJECT_RIGHT_READ,
                                           OBJECT_TYPE_NONE) == NULL,
                        "revoke parent and invalidate every descendant");
    ok &= objtest_check(!capability_revoke(&table, cap),
                        "reject revoking an already-revoked capability");
    ok &= objtest_check(!capability_allows(&table, cap, OBJECT_RIGHT_READ,
                                           OBJECT_TYPE_NONE),
                        "capability_allows rejects revoked capabilities");
    ok &= objtest_check(capability_derive(&table, cap, OBJECT_RIGHT_READ) == 0,
                        "reject derivation from a revoked parent");

    object_t *missing = capability_resolve(&table, 0x7fffffffu, OBJECT_RIGHT_READ,
                                          OBJECT_TYPE_NONE);
    ok &= objtest_check(missing == NULL,
                        "resolve rejects missing capability handles");
    ok &= objtest_check(!capability_allows(&table, 0x7fffffffu, OBJECT_RIGHT_READ,
                                           OBJECT_TYPE_NONE),
                        "capability_allows rejects missing capability handles");

    handle_table_close_all(&table);
    ok &= objtest_check(object_count() == before && g_objtest_destroyed == 1,
                        "release capabilities and target at final close");
    console_write(ok ? "  M6 capability self-test: PASS\n"
                     : "  M6 capability self-test: FAIL\n");
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
       wired up, a triple fault does the job on real hardware. */
    outb(0x64, 0xFE);
    cpu_disable_interrupts();
    for (;;) {
        cpu_halt();
    }
}

static void cmd_fsinfo(int argc, char **argv) {
    (void)argc; (void)argv;
    console_write("Helix native filesystem (helixfs)\n");
    console_write("Mounted at: /\n");
    console_write("Read-only boot filesystem\n");
    console_write("Files:\n");
    const char *root = fs_boot_root();
    if (root) {
        file_entry_t *dir = fs_open("/");
        if (dir) {
            file_entry_t *entries[32];
            uint64_t count = fs_list_dir(dir, entries, 32);
            for (uint64_t i = 0; i < count; i++) {
                file_entry_t *entry = entries[i];
                console_write("  ");
                if (fs_file_is_dir(entry)) {
                    console_set_fg(CONSOLE_BRIGHT_CYAN);
                    console_write("[DIR]  ");
                } else {
                    console_write("[FILE] ");
                }
                console_write(entry->name);
                if (!fs_file_is_dir(entry)) {
                    console_write(" (");
                    console_write_dec(fs_file_size(entry));
                    console_write(" bytes)");
                }
                console_putc('\n');
                console_reset_color();
            }
        }
    }
}

static uint64_t resolve_path(ext4_fs_t *fs, const char *path, ext4_stat_t *out);

static bool ls_ext4_cb(void *ctx, const ext4_stat_t *st,
                       const char *name, uint32_t name_len) {
    (void)ctx;
    if (name_len > 255) name_len = 255;
    char buf[256];
    memcpy(buf, name, name_len);
    buf[name_len] = '\0';
    if (EXT4_S_ISDIR(st->mode)) {
        console_set_fg(CONSOLE_BRIGHT_CYAN);
        console_write("[DIR]  ");
    } else {
        console_write("[FILE] ");
    }
    console_write(buf);
    console_putc('\n');
    console_reset_color();
    return false;
}

static void cmd_ls(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "/";

    /* ext4 root takes priority (normal boot path) */
    ext4_fs_t *e4 = ext4_get_current();
    if (e4) {
        ext4_stat_t st;
        uint64_t ino = resolve_path(e4, path, &st);
        if (ino && EXT4_S_ISDIR(st.mode)) {
            ext4_readdir(e4, ino, ls_ext4_cb, NULL);
        } else {
            console_write("ls: not a directory: ");
            console_write(path);
            console_putc('\n');
        }
        return;
    }

    /* Fallback: helixfs boot filesystem */
    file_entry_t *dir = fs_open(path);
    if (dir == NULL || !fs_file_is_dir(dir)) {
        console_write("ls: not a directory: ");
        console_write(path);
        console_putc('\n');
        return;
    }
    file_entry_t *entries[64];
    uint64_t count = fs_list_dir(dir, entries, 64);
    for (uint64_t i = 0; i < count; i++) {
        file_entry_t *entry = entries[i];
        if (fs_file_is_dir(entry)) {
            console_set_fg(CONSOLE_BRIGHT_CYAN);
            console_write("[DIR]  ");
        } else {
            console_write("[FILE] ");
        }
        console_write(entry->name);
        console_putc('\n');
        console_reset_color();
    }
}

static void cmd_cat(int argc, char **argv) {
    if (argc < 2) {
        console_write("cat: missing file operand\n");
        return;
    }
    file_entry_t *file = fs_open(argv[1]);
    if (file == NULL) {
        console_write("cat: ");
        console_write(argv[1]);
        console_write(": No such file\n");
        return;
    }
    if (fs_file_is_dir(file)) {
        console_write("cat: ");
        console_write(argv[1]);
        console_write(": Is a directory\n");
        return;
    }
    uint64_t size = fs_file_size(file);
    void *data = file_get_data(file);
    if (data && size > 0) {
        console_write((const char *)data);
    }
    if (size == 0 || ((const char *)data)[size - 1] != '\n') {
        console_putc('\n');
    }
}

/* Kernel commands need ext4 filesystem functions */
static uint64_t g_cwd_ino = EXT4_ROOT_INO;

/* Resolve path to inode, handling relative/absolute */
static uint64_t resolve_path(ext4_fs_t *fs, const char *path, ext4_stat_t *out) {
    if (!path || *path == '\0') return 0;
    uint64_t ino;
    if (*path == '/') {
        ino = EXT4_ROOT_INO;
        path++;
    } else {
        ino = g_cwd_ino;
    }
    ext4_stat_t st;
    if (!ext4_stat(fs, ino, &st) || !EXT4_S_ISDIR(st.mode)) return 0;
    if (*path == '\0') {
        if (out) *out = st;
        return ino;
    }
    while (*path) {
        const char *slash = strchr(path, '/');
        size_t len = slash ? (size_t)(slash - path) : strlen(path);
        if (len == 0) {
            path = slash ? slash + 1 : path + len;
            continue;
        }
        char name[EXT4_NAME_MAX + 1];
        if (len > EXT4_NAME_MAX) return 0;
        memcpy(name, path, len);
        name[len] = '\0';
        if (!ext4_lookup(fs, ino, name, &st)) return 0;
        if (!EXT4_S_ISDIR(st.mode)) {
            if (slash) return 0; /* not a dir but more path remains */
            if (out) *out = st;
            return st.ino;
        }
        ino = st.ino;
        path = slash ? slash + 1 : path + len;
    }
    if (out) *out = st;
    return ino;
}

static void cmd_cd(int argc, char **argv) {
    if (!ext4_get_current()) {
        console_write("cd: no ext4 filesystem mounted\n");
        return;
    }
    const char *path = argc > 1 ? argv[1] : "/";
    ext4_stat_t st;
    uint64_t ino = resolve_path(ext4_get_current(), path, &st);
    if (!ino || !EXT4_S_ISDIR(st.mode)) {
        console_write("cd: not a directory: ");
        console_write(path);
        console_putc('\n');
        return;
    }
    g_cwd_ino = ino;
}

static void cmd_pwd(int argc, char **argv) {
    (void)argc; (void)argv;
    if (!ext4_get_current()) {
        console_write("/ (no ext4 fs)\n");
        return;
    }
    /* For simplicity, we just show the current working directory inode.
       Full path reconstruction would require walking up parent directories. */
    console_write("/ (inode ");
    console_write_dec(g_cwd_ino);
    console_write(")\n");
}

static void cmd_mkdir(int argc, char **argv) {
    if (!ext4_get_current()) {
        console_write("mkdir: no ext4 filesystem mounted\n");
        return;
    }
    if (argc < 2) {
        console_write("mkdir: missing operand\n");
        return;
    }
    const char *path = argv[1];
    const char *slash = strrchr(path, '/');
    char dir_path[256] = "/";
    char name[EXT4_NAME_MAX + 1];
    uint64_t parent_ino;
    if (slash) {
        size_t dir_len = slash - path;
        if (dir_len > 0) {
            memcpy(dir_path, path, dir_len);
            dir_path[dir_len] = '\0';
        }
        size_t name_len = strlen(slash + 1);
        if (name_len > EXT4_NAME_MAX) {
            console_write("mkdir: name too long\n");
            return;
        }
        memcpy(name, slash + 1, name_len);
        name[name_len] = '\0';
    } else {
        size_t name_len = strlen(path);
        if (name_len > EXT4_NAME_MAX) {
            console_write("mkdir: name too long\n");
            return;
        }
        memcpy(name, path, name_len);
        name[name_len] = '\0';
    }
    ext4_stat_t st;
    parent_ino = resolve_path(ext4_get_current(), dir_path, &st);
    if (!parent_ino || !EXT4_S_ISDIR(st.mode)) {
        console_write("mkdir: parent directory not found\n");
        return;
    }
    uint64_t new_ino;
    if (!ext4_mkdir(ext4_get_current(), parent_ino, name, EXT4_S_IFDIR | 0755, &new_ino)) {
        console_write("mkdir: failed to create directory\n");
        return;
    }
    console_write("mkdir: created ");
    console_write(path);
    console_putc('\n');
}

static void cmd_rmdir(int argc, char **argv) {
    if (!ext4_get_current()) {
        console_write("rmdir: no ext4 filesystem mounted\n");
        return;
    }
    if (argc < 2) {
        console_write("rmdir: missing operand\n");
        return;
    }
    const char *path = argv[1];
    const char *slash = strrchr(path, '/');
    char dir_path[256] = "/";
    char name[EXT4_NAME_MAX + 1];
    uint64_t parent_ino;
    if (slash) {
        size_t dir_len = slash - path;
        if (dir_len > 0) {
            memcpy(dir_path, path, dir_len);
            dir_path[dir_len] = '\0';
        }
        size_t name_len = strlen(slash + 1);
        if (name_len > EXT4_NAME_MAX) {
            console_write("rmdir: name too long\n");
            return;
        }
        memcpy(name, slash + 1, name_len);
        name[name_len] = '\0';
    } else {
        size_t name_len = strlen(path);
        if (name_len > EXT4_NAME_MAX) {
            console_write("rmdir: name too long\n");
            return;
        }
        memcpy(name, path, name_len);
        name[name_len] = '\0';
    }
    ext4_stat_t st;
    parent_ino = resolve_path(ext4_get_current(), dir_path, &st);
    if (!parent_ino || !EXT4_S_ISDIR(st.mode)) {
        console_write("rmdir: parent directory not found\n");
        return;
    }
    if (!ext4_unlink(ext4_get_current(), parent_ino, name, true)) {
        console_write("rmdir: failed to remove directory\n");
        return;
    }
    console_write("rmdir: removed ");
    console_write(path);
    console_putc('\n');
}

static void cmd_touch(int argc, char **argv) {
    if (!ext4_get_current()) {
        console_write("touch: no ext4 filesystem mounted\n");
        return;
    }
    if (argc < 2) {
        console_write("touch: missing operand\n");
        return;
    }
    const char *path = argv[1];
    const char *slash = strrchr(path, '/');
    char dir_path[256] = "/";
    char name[EXT4_NAME_MAX + 1];
    uint64_t parent_ino;
    if (slash) {
        size_t dir_len = slash - path;
        if (dir_len > 0) {
            memcpy(dir_path, path, dir_len);
            dir_path[dir_len] = '\0';
        }
        size_t name_len = strlen(slash + 1);
        if (name_len > EXT4_NAME_MAX) {
            console_write("touch: name too long\n");
            return;
        }
        memcpy(name, slash + 1, name_len);
        name[name_len] = '\0';
    } else {
        size_t name_len = strlen(path);
        if (name_len > EXT4_NAME_MAX) {
            console_write("touch: name too long\n");
            return;
        }
        memcpy(name, path, name_len);
        name[name_len] = '\0';
    }
    ext4_stat_t st;
    parent_ino = resolve_path(ext4_get_current(), dir_path, &st);
    if (!parent_ino || !EXT4_S_ISDIR(st.mode)) {
        console_write("touch: parent directory not found\n");
        return;
    }
    uint64_t ino;
    if (ext4_lookup(ext4_get_current(), parent_ino, name, &st)) {
        /* File exists, just update timestamp (not fully implemented) */
        console_write("touch: ");
        console_write(path);
        console_write(" (updated)\n");
        return;
    }
    if (!ext4_create(ext4_get_current(), parent_ino, name, EXT4_S_IFREG | 0644, &ino)) {
        console_write("touch: failed to create file\n");
        return;
    }
    console_write("touch: created ");
    console_write(path);
    console_putc('\n');
}

static void cmd_rm(int argc, char **argv) {
    if (!ext4_get_current()) {
        console_write("rm: no ext4 filesystem mounted\n");
        return;
    }
    if (argc < 2) {
        console_write("rm: missing operand\n");
        return;
    }
    bool recursive = false;
    int arg_start = 1;
    if (argc > 2 && str_eq(argv[1], "-r")) {
        recursive = true;
        arg_start = 2;
    }
    for (int i = arg_start; i < argc; i++) {
        const char *path = argv[i];
        const char *slash = strrchr(path, '/');
        char dir_path[256] = "/";
        char name[EXT4_NAME_MAX + 1];
        uint64_t parent_ino;
        if (slash) {
            size_t dir_len = slash - path;
            if (dir_len > 0) {
                memcpy(dir_path, path, dir_len);
                dir_path[dir_len] = '\0';
            }
            size_t name_len = strlen(slash + 1);
            if (name_len > EXT4_NAME_MAX) {
                console_write("rm: name too long\n");
                continue;
            }
            memcpy(name, slash + 1, name_len);
            name[name_len] = '\0';
        } else {
            size_t name_len = strlen(path);
            if (name_len > EXT4_NAME_MAX) {
                console_write("rm: name too long\n");
                continue;
            }
            memcpy(name, path, name_len);
            name[name_len] = '\0';
        }
        ext4_stat_t st;
        parent_ino = resolve_path(ext4_get_current(), dir_path, &st);
        if (!parent_ino || !EXT4_S_ISDIR(st.mode)) {
            console_write("rm: parent directory not found\n");
            continue;
        }
        if (!ext4_lookup(ext4_get_current(), parent_ino, name, &st)) {
            console_write("rm: cannot remove '");
            console_write(path);
            console_write("': No such file\n");
            continue;
        }
        if (EXT4_S_ISDIR(st.mode) && !recursive) {
            console_write("rm: cannot remove '");
            console_write(path);
            console_write("': Is a directory (use -r)\n");
            continue;
        }
        if (EXT4_S_ISDIR(st.mode)) {
            if (!rm_remove_dir(ext4_get_current(), st.ino, parent_ino, name)) {
                console_write("rm: failed to remove '");
                console_write(path);
                console_write("'\n");
                continue;
            }
            continue;
        }
        if (!ext4_unlink(ext4_get_current(), parent_ino, name, false)) {
            console_write("rm: failed to remove '");
            console_write(path);
            console_write("'\n");
            continue;
        }
    }
}

/* One directory entry captured before we start mutating the tree.
 * ext4_readdir() walks the directory through the driver's single scratch
 * buffer, so the entries must be snapshotted before any create/write/unlink
 * call clobbers that buffer mid-walk. */
typedef struct {
    char name[EXT4_NAME_MAX + 1];
    uint16_t mode;
    uint64_t ino;
    uint64_t size;
} dir_entry_t;

typedef struct {
    dir_entry_t *entries;
    size_t count;
    size_t cap;
} dir_list_t;

static void dir_list_free(dir_list_t *l) {
    if (l->entries) kfree(l->entries);
    l->entries = NULL;
    l->count = 0;
    l->cap = 0;
}

/* Count entries; callback returns false to keep scanning (see ext4_dirent_cb contract: true stops). */
static bool dir_list_count_cb(void *ctx, const ext4_stat_t *st,
                              const char *name, uint32_t name_len) {
    (void)ctx; (void)st; (void)name; (void)name_len;
    return false;
}

/* Snapshot one entry; return false to keep scanning. */
static bool dir_list_fill_cb(void *ctx, const ext4_stat_t *st,
                             const char *name, uint32_t name_len) {
    dir_list_t *l = ctx;
    if (name_len > EXT4_NAME_MAX) return false;
    if (l->entries == NULL || l->count >= l->cap) return true; /* stop when full */
    dir_entry_t *e = &l->entries[l->count++];
    memcpy(e->name, name, name_len);
    e->name[name_len] = '\0';
    e->mode = st->mode;
    e->ino = st->ino;
    e->size = st->size;
    return false;
}

/* Two-pass snapshot into heap so we never put a large dir on the 32KB kernel stack. */
static bool dir_snapshot(ext4_fs_t *fs, uint64_t dir_ino, dir_list_t *out) {
    out->entries = NULL;
    out->count = 0;
    out->cap = 0;

    uint64_t n = ext4_readdir(fs, dir_ino, dir_list_count_cb, NULL);
    if (n == 0) return true;
    out->entries = kmalloc((size_t)n * sizeof(dir_entry_t));
    if (!out->entries) return false;
    out->cap = (size_t)n;
    ext4_readdir(fs, dir_ino, dir_list_fill_cb, out);
    return true;
}

/* Copy a regular file's contents into a freshly created file. */
static bool copy_file_contents(ext4_fs_t *fs, uint64_t src_ino, uint64_t dst_ino,
                               uint64_t size) {
    char *buf = kmalloc(size + 1);
    if (!buf) return false;
    if (ext4_read(fs, src_ino, 0, buf, size) != size) {
        kfree(buf);
        return false;
    }
    bool ok = (ext4_write(fs, dst_ino, 0, buf, size) == size);
    kfree(buf);
    return ok;
}

/* Recursively copy a directory tree. */
static bool cp_copy_dir(ext4_fs_t *fs, uint64_t src_ino, uint64_t dst_parent,
                        const char *dst_name) {
    ext4_stat_t st;
    if (!ext4_stat(fs, src_ino, &st)) return false;

    uint64_t new_dir_ino;
    if (!ext4_mkdir(fs, dst_parent, dst_name, st.mode, &new_dir_ino)) {
        return false;
    }

    dir_list_t list;
    if (!dir_snapshot(fs, src_ino, &list)) {
        return false;
    }

    bool ok = true;
    for (size_t i = 0; i < list.count; i++) {
        dir_entry_t *e = &list.entries[i];
        if (EXT4_S_ISDIR(e->mode)) {
            if (!cp_copy_dir(fs, e->ino, new_dir_ino, e->name)) { ok = false; break; }
        } else if (EXT4_S_ISREG(e->mode)) {
            uint64_t file_ino;
            if (!ext4_create(fs, new_dir_ino, e->name, e->mode, &file_ino) ||
                !copy_file_contents(fs, e->ino, file_ino, e->size)) {
                ok = false;
                break;
            }
        }
    }
    dir_list_free(&list);
    return ok;
}
static void cmd_cp(int argc, char **argv) {
    if (!ext4_get_current()) {
        console_write("cp: no ext4 filesystem mounted\n");
        return;
    }
    if (argc < 3) {
        console_write("cp: missing operand\n");
        return;
    }

    bool recursive = false;
    int arg_start = 1;
    if (argc > 3 && str_eq(argv[1], "-r")) {
        recursive = true;
        arg_start = 2;
    }

    const char *src = argv[arg_start];
    const char *dst = argv[arg_start + 1];
    ext4_stat_t st;
    uint64_t src_ino = resolve_path(ext4_get_current(), src, &st);
    if (!src_ino) {
        console_write("cp: cannot stat '");
        console_write(src);
        console_write("'\n");
        return;
    }
    uint64_t src_size = st.size; /* capture before st is clobbered by dst dir resolve */

    if (EXT4_S_ISDIR(st.mode)) {
        if (!recursive) {
            console_write("cp: omitting directory '");
            console_write(src);
            console_write("' (use -r to copy recursively)\n");
            return;
        }
        /* Recursive directory copy */
        const char *slash = strrchr(dst, '/');
        char dir_path[256] = "/";
        char name[EXT4_NAME_MAX + 1];
        uint64_t parent_ino;
        if (slash) {
            size_t dir_len = slash - dst;
            if (dir_len > 0) {
                memcpy(dir_path, dst, dir_len);
                dir_path[dir_len] = '\0';
            }
            size_t name_len = strlen(slash + 1);
            if (name_len > EXT4_NAME_MAX) {
                console_write("cp: name too long\n");
                return;
            }
            memcpy(name, slash + 1, name_len);
            name[name_len] = '\0';
        } else {
            size_t name_len = strlen(dst);
            if (name_len > EXT4_NAME_MAX) {
                console_write("cp: name too long\n");
                return;
            }
            memcpy(name, dst, name_len);
            name[name_len] = '\0';
        }
        parent_ino = resolve_path(ext4_get_current(), dir_path, &st);
        if (!parent_ino || !EXT4_S_ISDIR(st.mode)) {
            console_write("cp: parent directory not found\n");
            return;
        }
        if (!cp_copy_dir(ext4_get_current(), src_ino, parent_ino, name)) {
            console_write("cp: failed to copy directory\n");
            return;
        }
        console_write("cp: copied directory ");
        console_write(src);
        console_write(" -> ");
        console_write(dst);
        console_putc('\n');
        return;
    }
    /* Read source */
    char *buf = kmalloc(src_size + 1);
    if (!buf) {
        console_write("cp: out of memory\n");
        return;
    }
    uint64_t read = ext4_read(ext4_get_current(), src_ino, 0, buf, src_size);
    if (read != src_size) {
        kfree(buf);
        return;
    }
    /* Create destination */
    const char *slash = strrchr(dst, '/');
    char dir_path[256] = "/";
    char name[EXT4_NAME_MAX + 1];
    uint64_t parent_ino;
    if (slash) {
        size_t dir_len = slash - dst;
        if (dir_len > 0) {
            memcpy(dir_path, dst, dir_len);
            dir_path[dir_len] = '\0';
        }
        size_t name_len = strlen(slash + 1);
        if (name_len > EXT4_NAME_MAX) {
            console_write("cp: name too long\n");
            kfree(buf);
            return;
        }
        memcpy(name, slash + 1, name_len);
        name[name_len] = '\0';
    } else {
        size_t name_len = strlen(dst);
        if (name_len > EXT4_NAME_MAX) {
            console_write("cp: name too long\n");
            kfree(buf);
            return;
        }
        memcpy(name, dst, name_len);
        name[name_len] = '\0';
    }
    parent_ino = resolve_path(ext4_get_current(), dir_path, &st);
    if (!parent_ino || !EXT4_S_ISDIR(st.mode)) {
        console_write("cp: parent directory not found\n");
        kfree(buf);
        return;
    }
    uint64_t dst_ino;
    if (!ext4_create(ext4_get_current(), parent_ino, name, EXT4_S_IFREG | 0644, &dst_ino)) {
        console_write("cp: failed to create destination\n");
        kfree(buf);
        return;
    }
    if (ext4_write(ext4_get_current(), dst_ino, 0, buf, src_size) != src_size) {
        console_write("cp: write error\n");
        kfree(buf);
        return;
    }
    console_write("cp: copied ");
    console_write(src);
    console_write(" -> ");
    console_write(dst);
    console_putc('\n');
    kfree(buf);
}

/* Recursively remove a directory tree (for mv cleanup). */
static bool rm_remove_dir(ext4_fs_t *fs, uint64_t dir_ino, uint64_t parent_ino,
                          const char *name) {
    ext4_stat_t st;
    if (!ext4_stat(fs, dir_ino, &st)) return false;

    dir_list_t list;
    if (!dir_snapshot(fs, dir_ino, &list)) {
        return false;
    }

    bool ok = true;
    for (size_t i = 0; i < list.count; i++) {
        dir_entry_t *e = &list.entries[i];
        if (EXT4_S_ISDIR(e->mode)) {
            if (!rm_remove_dir(fs, e->ino, dir_ino, e->name)) { ok = false; break; }
        } else if (EXT4_S_ISREG(e->mode)) {
            if (!ext4_unlink(fs, dir_ino, e->name, false)) { ok = false; break; }
        }
    }
    dir_list_free(&list);
    if (!ok) return false;
    return ext4_unlink(fs, parent_ino, name, true);
}

static void cmd_mv(int argc, char **argv) {
    if (!ext4_get_current()) {
        console_write("mv: no ext4 filesystem mounted\n");
        return;
    }
    if (argc < 3) {
        console_write("mv: missing operand\n");
        return;
    }
    const char *src = argv[1];
    const char *dst = argv[2];
    ext4_stat_t st;
    uint64_t src_ino = resolve_path(ext4_get_current(), src, &st);
    if (!src_ino) {
        console_write("mv: cannot stat '");
        console_write(src);
        console_write("'\n");
        return;
    }
    uint64_t src_size = st.size; /* preserve before st is clobbered */

    const char *slash = strrchr(dst, '/');
    char dir_path[256] = "/";
    char name[EXT4_NAME_MAX + 1];
    uint64_t dst_parent;
    if (slash) {
        size_t dir_len = slash - dst;
        if (dir_len > 0) {
            memcpy(dir_path, dst, dir_len);
            dir_path[dir_len] = '\0';
        }
        size_t name_len = strlen(slash + 1);
        if (name_len > EXT4_NAME_MAX) {
            console_write("mv: name too long\n");
            return;
        }
        memcpy(name, slash + 1, name_len);
        name[name_len] = '\0';
    } else {
        size_t name_len = strlen(dst);
        if (name_len > EXT4_NAME_MAX) {
            console_write("mv: name too long\n");
            return;
        }
        memcpy(name, dst, name_len);
        name[name_len] = '\0';
    }
    ext4_stat_t dst_st;
    dst_parent = resolve_path(ext4_get_current(), dir_path, &dst_st);
    if (!dst_parent || !EXT4_S_ISDIR(dst_st.mode)) {
        console_write("mv: parent directory not found\n");
        return;
    }
    /* Check if destination exists and is directory */
    ext4_stat_t lookup_st;
    if (ext4_lookup(ext4_get_current(), dst_parent, name, &lookup_st) && EXT4_S_ISDIR(lookup_st.mode)) {
        /* Move into directory: use source basename */
        const char *src_slash = strrchr(src, '/');
        const char *src_name = src_slash ? src_slash + 1 : src;
        size_t name_len = strlen(src_name);
        if (name_len > EXT4_NAME_MAX) {
            console_write("mv: name too long\n");
            return;
        }
        memcpy(name, src_name, name_len);
        name[name_len] = '\0';
    }

    if (EXT4_S_ISDIR(st.mode)) {
        /* Recursive directory move: copy + remove */
        if (!cp_copy_dir(ext4_get_current(), src_ino, dst_parent, name)) {
            console_write("mv: failed to copy directory\n");
            return;
        }
        /* Remove source directory recursively */
        const char *src_slash = strrchr(src, '/');
        char src_dir[256] = "/";
        char src_name[EXT4_NAME_MAX + 1];
        uint64_t src_parent;
        if (src_slash) {
            size_t dir_len = src_slash - src;
            if (dir_len > 0) {
                memcpy(src_dir, src, dir_len);
                src_dir[dir_len] = '\0';
            }
            size_t name_len = strlen(src_slash + 1);
            memcpy(src_name, src_slash + 1, name_len);
            src_name[name_len] = '\0';
        } else {
            size_t name_len = strlen(src);
            memcpy(src_name, src, name_len);
            src_name[name_len] = '\0';
        }
        src_parent = resolve_path(ext4_get_current(), src_dir, &st);
        if (src_parent && EXT4_S_ISDIR(st.mode)) {
            rm_remove_dir(ext4_get_current(), src_ino, src_parent, src_name);
        }
        console_write("mv: moved ");
        console_write(src);
        console_write(" -> ");
        console_write(dst);
        console_putc('\n');
        return;
    }
    /* Read source */
    char *buf = kmalloc(src_size + 1);
    if (!buf) {
        console_write("mv: out of memory\n");
        return;
    }
    uint64_t read = ext4_read(ext4_get_current(), src_ino, 0, buf, src_size);
    if (read != src_size) {
        console_write("mv: read error\n");
        kfree(buf);
        return;
    }
    uint64_t dst_ino;
    if (!ext4_create(ext4_get_current(), dst_parent, name, EXT4_S_IFREG | 0644, &dst_ino)) {
        console_write("mv: failed to create destination\n");
        kfree(buf);
        return;
    }
    if (ext4_write(ext4_get_current(), dst_ino, 0, buf, src_size) != src_size) {
        console_write("mv: write error\n");
        kfree(buf);
        return;
    }
    /* Unlink source */
    const char *src_slash = strrchr(src, '/');
    char src_dir[256] = "/";
    char src_name[EXT4_NAME_MAX + 1];
    uint64_t src_parent;
    if (src_slash) {
        size_t dir_len = src_slash - src;
        if (dir_len > 0) {
            memcpy(src_dir, src, dir_len);
            src_dir[dir_len] = '\0';
        }
        size_t name_len = strlen(src_slash + 1);
        memcpy(src_name, src_slash + 1, name_len);
        src_name[name_len] = '\0';
    } else {
        size_t name_len = strlen(src);
        memcpy(src_name, src, name_len);
        src_name[name_len] = '\0';
    }
    src_parent = resolve_path(ext4_get_current(), src_dir, &st);
    if (src_parent && EXT4_S_ISDIR(st.mode)) {
        ext4_unlink(ext4_get_current(), src_parent, src_name, false);
    }
    console_write("mv: moved ");
    console_write(src);
    console_write(" -> ");
    console_write(dst);
    console_putc('\n');
    kfree(buf);
}

static void cmd_df(int argc, char **argv) {
    (void)argc; (void)argv;
    if (!ext4_get_current()) {
        console_write("df: no ext4 filesystem mounted\n");
        return;
    }
    uint64_t total_b, free_b, total_i, free_i;
    ext4_space(ext4_get_current(), &total_b, &free_b, &total_i, &free_i);
    console_write("Filesystem     Blocks      Free   Inodes   Free\n");
    console_write("ext4-root  ");
    print_u64(total_b / 1024);
    console_write("  ");
    print_u64(free_b / 1024);
    console_write("  ");
    print_u64(total_i);
    console_write("  ");
    print_u64(free_i);
    console_putc('\n');
}

static void cmd_exit(int argc, char **argv) {
    (void)argc; (void)argv;
    console_write("Use 'halt' to stop the machine.\n");
}

static void cmd_uname(int argc, char **argv) {
    (void)argc; (void)argv;
    console_write("Helix OS\n");
}

static void cmd_whoami(int argc, char **argv) {
    (void)argc; (void)argv;
    console_write("root\n");
}

static void cmd_chmod(int argc, char **argv) {
    if (!ext4_get_current()) {
        console_write("chmod: no ext4 filesystem mounted\n");
        return;
    }
    if (argc < 3) {
        console_write("chmod: missing operand\n");
        console_write("usage: chmod <mode> <file>\n");
        return;
    }
    uint64_t mode;
    if (!parse_u64(argv[1], &mode)) {
        console_write("chmod: invalid mode\n");
        return;
    }
    const char *path = argv[2];
    ext4_stat_t st;
    uint64_t ino = resolve_path(ext4_get_current(), path, &st);
    if (!ino) {
        console_write("chmod: cannot access '");
        console_write(path);
        console_write("': No such file or directory\n");
        return;
    }
    /* ext4_truncate with new mode is a simplified implementation.
     * Real implementation would use chmod syscall / inode update. */
    console_write("chmod: mode change not fully supported in ext4 driver\n");
}

static void cmd_chown(int argc, char **argv) {
    if (!ext4_get_current()) {
        console_write("chown: no ext4 filesystem mounted\n");
        return;
    }
    if (argc < 3) {
        console_write("chown: missing operand\n");
        console_write("usage: chown <owner>[:<group>] <file>\n");
        return;
    }
    const char *path = argv[2];
    ext4_stat_t st;
    uint64_t ino = resolve_path(ext4_get_current(), path, &st);
    if (!ino) {
        console_write("chown: cannot access '");
        console_write(path);
        console_write("': No such file or directory\n");
        return;
    }
    /* Simplified - user/group management not fully implemented */
    console_write("chown: user management not implemented\n");
}

static void cmd_mkusr(int argc, char **argv) {
    if (argc < 2) {
        console_write("mkusr: missing username\n");
        console_write("usage: mkusr <username> [password]\n");
        return;
    }
    /* User management requires /etc/passwd and /etc/shadow support.
     * For now, just report the request. */
    console_write("mkusr: user '");
    console_write(argv[1]);
    console_write("' creation requested (not yet implemented)\n");
}

static void cmd_su(int argc, char **argv) {
    if (argc < 2) {
        console_write("su: missing username\n");
        console_write("usage: su [username]\n");
        return;
    }
    console_write("su: switching to user '");
    console_write(argv[1]);
    console_write("' (not yet implemented)\n");
}

static void cmd_dosu(int argc, char **argv) {
    if (argc < 3) {
        console_write("dosu: missing username or command\n");
        console_write("usage: dosu <username> <command> [args...]\n");
        return;
    }
    console_write("dosu: would execute '");
    for (int i = 2; i < argc; i++) {
        if (i > 2) console_putc(' ');
        console_write(argv[i]);
    }
    console_write("' as user '");
    console_write(argv[1]);
    console_write("' (not yet implemented)\n");
}

static bool list_dir_cb(void *ctx, const ext4_stat_t *st,
                        const char *name, uint32_t name_len);

static void cmd_find(int argc, char **argv) {
    if (!ext4_get_current()) {
        console_write("find: no ext4 filesystem mounted\n");
        return;
    }
    if (argc < 2) {
        console_write("find: missing path\n");
        console_write("usage: find <path>\n");
        return;
    }
    const char *path = argv[1];
    ext4_stat_t st;
    uint64_t ino = resolve_path(ext4_get_current(), path, &st);
    if (!ino) {
        console_write("find: '");
        console_write(path);
        console_write("': No such file or directory\n");
        return;
    }
    if (!EXT4_S_ISDIR(st.mode)) {
        console_write("find: '");
        console_write(path);
        console_write("': Not a directory\n");
        return;
    }
    /* Simple directory listing */
    ext4_readdir(ext4_get_current(), ino, list_dir_cb, NULL);
}

static bool list_dir_cb(void *ctx, const ext4_stat_t *st,
                        const char *name, uint32_t name_len) {
    (void)ctx;
    if (name_len > 255) name_len = 255;
    char buf[256];
    memcpy(buf, name, name_len);
    buf[name_len] = '\0';
    console_write(buf);
    if (EXT4_S_ISDIR(st->mode)) console_write("/");
    console_putc('\n');
    return false;  /* continue */
}

static void cmd_free(int argc, char **argv) {
    (void)argc; (void)argv;
    console_write("              total        used        free      shared  buff/cache   available\n");
    kprintf("Mem:  %10llu %10llu %10llu %10llu %10llu %10llu\n",
            (unsigned long long)(memory_usable_bytes() / 1024),
            (unsigned long long)((memory_usable_bytes() - pmm_free_bytes()) / 1024),
            (unsigned long long)(pmm_free_bytes() / 1024),
            0ULL, 0ULL,
            (unsigned long long)(pmm_free_bytes() / 1024));
    kprintf("Swap: %10llu %10llu %10llu\n", 0ULL, 0ULL, 0ULL);
}

static void print_padded2(uint64_t value) {
    if (value < 10) console_putc('0');
    console_write_dec(value);
}

static void cmd_date(int argc, char **argv) {
    (void)argc; (void)argv;
    int year, month, day, hour, minute, second, weekday;
    time_now_utc(&year, &month, &day, &hour, &minute, &second, &weekday);
    static const char *const wd[7] = {
        "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
    };
    static const char *const mn[12] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    if (year == 0) {
        console_write("date: no wall clock available\n");
        return;
    }
    console_write(wd[(weekday >= 0 && weekday < 7) ? weekday : 0]);
    console_putc(' ');
    console_write(mn[(month >= 1 && month <= 12) ? month - 1 : 0]);
    console_putc(' ');
    if (day < 10) console_putc(' ');
    console_write_dec(day);
    console_putc(' ');
    print_padded2(hour);
    console_putc(':');
    print_padded2(minute);
    console_putc(':');
    print_padded2(second);
    console_putc(' ');
    console_write("UTC ");
    console_write_dec(year);
    console_putc('\n');
}

static void cmd_time(int argc, char **argv) {
    (void)argc; (void)argv;
    int year, month, day, hour, minute, second, weekday;
    time_now_utc(&year, &month, &day, &hour, &minute, &second, &weekday);
    if (year == 0) {
        console_write("time: no wall clock available\n");
        return;
    }
    print_padded2(hour);
    console_putc(':');
    print_padded2(minute);
    console_putc(':');
    print_padded2(second);
    console_putc('\n');
}

static void cmd_ps(int argc, char **argv) {
    (void)argc; (void)argv;
    console_write("PID  TTY          TIME CMD\n");
    kprintf("  0  kernel      00:00:00 [kernel]\n");
    kprintf("  1  kernel      00:00:00 [thread_receiver]\n");
    kprintf("  2  kernel      00:00:00 [thread_sender]\n");
    /* TODO: walk thread list and show user processes */
}

static void cmd_kill(int argc, char **argv) {
    (void)argc; (void)argv;
    console_write("kill: process management not implemented (no user processes yet)\n");
}

static void cmd_mount(int argc, char **argv) {
    if (argc < 3) {
        console_write("mount: missing operand\n");
        console_write("usage: mount <device> <mountpoint> [-t fstype]\n");
        return;
    }
    console_write("mount: ");
    console_write(argv[1]);
    console_write(" on ");
    console_write(argv[2]);
    console_write(" (not yet implemented)\n");
}

static void cmd_umount(int argc, char **argv) {
    if (argc < 2) {
        console_write("umount: missing mountpoint\n");
        console_write("usage: umount <mountpoint>\n");
        return;
    }
    console_write("umount: ");
    console_write(argv[1]);
    console_write(" (not yet implemented)\n");
}

static void cmd_true(int argc, char **argv) {
    (void)argc; (void)argv;
    console_write("true: returning success (exit code 0)\n");
    return;
}

static void cmd_false(int argc, char **argv) {
    (void)argc; (void)argv;
    console_write("false: returning failure (exit code 1)\n");
    return;
}

/* The table */
const command_t helix_commands[] = {
    {"help",    "list available commands",              cmd_help},
    {"sysinfo", "system information (Neofetch-style)",  cmd_sysinfo},
    {"neofetch","alias for sysinfo",                    cmd_neofetch},
    {"about",   "alias for sysinfo",                    cmd_about},
    {"cpu",     "show CPU vendor and brand",            cmd_cpu},
    {"mem",     "show the physical memory map",         cmd_mem},
    {"pmm",     "physical frame allocator statistics",   cmd_pmm},
    {"heap",    "kernel heap statistics",                cmd_heap},
    {"memtest", "exercise kmalloc/kfree",                cmd_memtest},
    {"vmtest",  "exercise map/unmap/protect",            cmd_vmtest},
    {"stacktest","verify guarded NX kernel stack",       cmd_stacktest},
    {"objects", "show object manager statistics",        cmd_objects},
    {"objtest", "exercise objects and handles",          cmd_objtest},
    {"captest", "exercise capability rights and revocation", cmd_captest},
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
    {"fsinfo",  "show filesystem information",          cmd_fsinfo},
    {"ls",      "list directory contents",              cmd_ls},
    {"cat",     "display file contents",                cmd_cat},
    {"cd",      "change working directory",             cmd_cd},
    {"pwd",     "print working directory",              cmd_pwd},
    {"mkdir",   "create a directory",                   cmd_mkdir},
    {"rmdir",   "remove a directory",                   cmd_rmdir},
    {"touch",   "create or update a file",              cmd_touch},
    {"rm",      "remove files",                         cmd_rm},
    {"cp",      "copy a file (-r for directories)",     cmd_cp},
    {"mv",      "move or rename a file/directory",      cmd_mv},
    {"df",      "show disk free space",                 cmd_df},
    {"uname",   "show OS name",                         cmd_uname},
    {"whoami",  "show current user",                    cmd_whoami},
    {"chmod",   "change file mode bits",                cmd_chmod},
    {"chown",   "change file owner",                    cmd_chown},
    {"mkusr",   "create a user account",                cmd_mkusr},
    {"su",      "switch user identity",                 cmd_su},
    {"dosu",    "switch user and execute command",       cmd_dosu},
    {"find",    "search for files in a directory tree", cmd_find},
    {"exit",    "halt the machine",                     cmd_exit},
    {"free",    "show free memory summary",              cmd_free},
    {"date",    "show current date and time",            cmd_date},
    {"time",    "show current system time",              cmd_time},
    {"ps",      "show running processes",               cmd_ps},
    {"kill",    "terminate a process",                   cmd_kill},
    {"mount",   "mount a filesystem",                   cmd_mount},
    {"umount",  "unmount a filesystem",                 cmd_umount},
    {"true",    "do nothing, successfully",              cmd_true},
    {"false",   "do nothing, unsuccessfully",           cmd_false},
};

const size_t helix_command_count = sizeof(helix_commands) / sizeof(helix_commands[0]);