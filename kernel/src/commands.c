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
#include "boot_app.h"
#include "channel.h"
#include "console.h"
#include "cpu.h"
#include "driver.h"
#include "elf.h"
#include "heap.h"
#include "io.h"
#include "kernel_stack.h"
#include "kstring.h"
#include "memory.h"
#include "object.h"
#include "panic.h"
#include "pci.h"
#include "pit.h"
#include "pmm.h"
#include "sysinfo.h"
#include "thread.h"
#include "time.h"
#include "version.h"
#include "usermode.h"
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

static void cmd_drivers(int argc, char **argv) {
    (void)argc;
    (void)argv;
    console_write("Registered device drivers:\n");
    for (size_t i = 0; i < driver_count(); i++) {
        driver_info_t info;
        if (!driver_get_info(i, &info)) continue;
        console_write("  ");
        console_write(info.name);
        console_write("  class=");
        console_write(info.device_class);
        console_write(info.active ? "  active" : "  discovered");
        console_write("  device=");
        console_write_dec(info.object_id);
        console_write("  control-cap=");
        console_write_dec(info.control_handle);
        console_putc('\n');
    }
}

static void cmd_usb(int argc, char **argv) {
    (void)argc;
    (void)argv;
    pci_print_usb_controllers();
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

static void cmd_asptest(int argc, char **argv) {
    (void)argc;
    (void)argv;
    console_write("Address-space self-test\n");
    const uint64_t user_page = 0x00400000ULL;
    const uint64_t pattern = 0x48454C4958555345ULL;
    vmm_address_space_t space = {0};
    uintptr_t frame = pmm_alloc_frame();
    bool created = frame != 0 && vmm_address_space_create(&space);
    bool ok = objtest_check(created,
                            "create private lower-half page-table root");
    if (!created) {
        if (frame != 0) pmm_free_frame(frame);
        console_write("  M9 address-space self-test: FAIL\n");
        return;
    }

    uint64_t kernel_phys_before = 0;
    uint64_t kernel_flags_before = 0;
    bool kernel_had_mapping = vmm_get_mapping(user_page, &kernel_phys_before,
                                               &kernel_flags_before);
    bool initially_absent = !vmm_address_space_get_mapping(&space, user_page,
                                                            NULL, NULL);
    bool mapped = vmm_address_space_map_user(&space, user_page, frame,
        VMM_PRESENT | VMM_WRITE | VMM_USER | VMM_NX);
    uint64_t mapped_phys = 0;
    uint64_t mapped_flags = 0;
    bool mapping_ok = initially_absent && mapped &&
        vmm_address_space_get_mapping(&space, user_page, &mapped_phys,
                                      &mapped_flags) &&
        mapped_phys == frame && (mapped_flags & (VMM_USER | VMM_WRITE | VMM_NX)) ==
                                  (VMM_USER | VMM_WRITE | VMM_NX);
    ok &= objtest_check(mapping_ok,
                        "map a user, writable, non-executable page");

    bool switched = mapped && vmm_address_space_switch(&space);
    uint64_t kernel_code_flags = 0;
    bool kernel_shared = switched &&
        (read_cr3() & 0x000FFFFFFFFFF000ULL) == space.root_phys &&
        vmm_address_space_get_mapping(&space,
            (uint64_t)(uintptr_t)&cmd_asptest, NULL, &kernel_code_flags) &&
        !(kernel_code_flags & VMM_USER);
    bool data_access = false;
    if (switched && mapping_ok) {
        volatile uint64_t *user_memory = (volatile uint64_t *)(uintptr_t)user_page;
        *user_memory = pattern;
        data_access = *user_memory == pattern;
    }
    ok &= objtest_check(kernel_shared && data_access,
                        "switch roots, keep kernel supervisor mappings, access user page");

    bool restored = vmm_address_space_switch(vmm_kernel_address_space());
    uint64_t kernel_phys_after = 0;
    uint64_t kernel_flags_after = 0;
    bool kernel_unchanged = restored &&
        vmm_get_mapping(user_page, &kernel_phys_after, &kernel_flags_after) ==
            kernel_had_mapping &&
        (!kernel_had_mapping || kernel_phys_after == kernel_phys_before);
    ok &= objtest_check(kernel_unchanged,
                        "private user mapping leaves kernel root unchanged");

    bool unmapped = mapped && vmm_address_space_unmap_user(&space, user_page);
    pmm_free_frame(frame);
    bool destroyed = vmm_address_space_destroy(&space);
    ok &= objtest_check(unmapped && destroyed,
                        "unmap user page and reclaim private page tables");
    console_write(ok ? "  M9 address-space self-test: PASS\n"
                     : "  M9 address-space self-test: FAIL\n");
}

static void cmd_usermodetest(int argc, char **argv) {
    (void)argc;
    (void)argv;
    console_write("Ring-3 transition self-test\n");
    uint64_t result = 0;
    bool ok = usermode_test(&result);
    console_write("  user program entered ring 3 and exited through int 0x80: ");
    console_write(ok ? "ok\n" : "FAILED\n");
    if (ok) {
        console_write("  user exit value: ");
        console_write_dec(result);
        console_putc('\n');
    }
    console_write(ok ? "  M9 ring-3 self-test: PASS\n"
                     : "  M9 ring-3 self-test: FAIL\n");
    uint64_t fault_vector = 0;
    bool fault_ok = usermode_fault_test(&fault_vector);
    console_write("  user exception containment: ");
    console_write(fault_ok ? "ok (exception " : "FAILED (exception ");
    console_write_dec(fault_vector);
    console_write(")\n");
    console_write(fault_ok ? "  M9 user-fault self-test: PASS\n"
                           : "  M9 user-fault self-test: FAIL\n");
}

static void cmd_drivercaptest(int argc, char **argv) {
    (void)argc;
    (void)argv;
    console_write("User-space driver capability isolation test\n");
    bool ok = usermode_driver_capability_test();
    console_write("  process without USB capability cannot obtain or forge READ: ");
    console_write(ok ? "ok\n" : "FAILED\n");
    console_write(ok ? "  M10 capability isolation test: PASS\n"
                     : "  M10 capability isolation test: FAIL\n");
}

static void cmd_runapp(int argc, char **argv) {
    const void *data = NULL;
    size_t size = 0;
    const char *app_name = NULL;
    const char *requested = argc > 1 ? argv[1] : NULL;
    if (!boot_app_get(requested, &app_name, &data, &size)) {
        console_write(requested == NULL ? "No .hlx application module was provided at boot.\n"
                                        : "No matching .hlx application module: ");
        if (requested != NULL) { console_write(requested); console_putc('\n'); }
        return;
    }

    enum { APP_STACK_PAGES = 4 };
    const uint64_t stack_top = 0x00007fffffffe000ULL;
    const uint64_t stack_base = stack_top - APP_STACK_PAGES * PAGE_SIZE;
    vmm_address_space_t space = {0};
    elf64_image_t image = {0};
    uintptr_t stack_frames[APP_STACK_PAGES] = {0};
    size_t stack_mapped = 0;
    bool loaded = false;
    bool ok = vmm_address_space_create(&space);
    if (ok) {
        loaded = elf64_load(&space, data, size, &image);
        ok = loaded;
    }
    while (ok && stack_mapped < APP_STACK_PAGES) {
        uintptr_t frame = pmm_alloc_frame();
        if (frame == 0 || !vmm_address_space_map_user(&space,
                stack_base + stack_mapped * PAGE_SIZE, frame,
                VMM_PRESENT | VMM_WRITE | VMM_USER | VMM_NX)) {
            if (frame != 0) pmm_free_frame(frame);
            ok = false;
            break;
        }
        stack_frames[stack_mapped++] = frame;
    }
    if (ok && vmm_address_space_switch(&space)) {
        memset((void *)(uintptr_t)stack_base, 0,
               APP_STACK_PAGES * PAGE_SIZE);
        ok = vmm_address_space_switch(vmm_kernel_address_space());
    } else if (ok) {
        ok = false;
    }

    uint64_t result = 0;
    uint64_t fault_vector = 0;
    if (ok) {
        console_write("Launching ");
        console_write(app_name);
        console_write(" as a native .hlx app...\n");
        if (str_eq(app_name, "shell.hlx")) {
            usermode_grant_next_uart_control();
            usermode_grant_next_usb_read();
        }
        ok = usermode_run(&space, image.entry, stack_top - 8, &result,
                          &fault_vector);
    }
    if ((read_cr3() & 0x000FFFFFFFFFF000ULL) !=
        (vmm_kernel_address_space()->root_phys & 0x000FFFFFFFFFF000ULL)) {
        vmm_address_space_switch(vmm_kernel_address_space());
    }
    for (size_t i = 0; i < stack_mapped; i++) {
        vmm_address_space_unmap_user(&space, stack_base + i * PAGE_SIZE);
        pmm_free_frame(stack_frames[i]);
    }
    if (loaded) elf64_unload(&space, &image);
    if (space.root_phys != 0) vmm_address_space_destroy(&space);

    if (ok && fault_vector != 0) {
        console_write("Application stopped after user exception ");
        console_write_dec(fault_vector);
        console_putc('\n');
    } else if (ok) {
        console_write("Application exited with status ");
        console_write_dec(result);
        console_putc('\n');
    } else {
        console_write("Application load or execution failed.\n");
    }
}

bool commands_start_init(void) {
    const void *data = NULL;
    size_t size = 0;
    if (!boot_app_get("shell.hlx", NULL, &data, &size)) return false;
    (void)data;
    (void)size;
    char *argv[] = { "runapp", "shell.hlx" };
    cmd_runapp(2, argv);
    return true;
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

static void cmd_chantest(int argc, char **argv) {
    (void)argc;
    (void)argv;
    console_write("IPC channel self-test\n");

    uint64_t before = object_count();
    handle_table_t table;
    handle_table_init(&table);
    handle_t send_handle = 0;
    handle_t receive_handle = 0;
    bool created = channel_create_pair(&table, 1, &send_handle, &receive_handle);
    bool ok = objtest_check(created && send_handle != 0 && receive_handle != 0,
                            "create bounded send/receive endpoints");
    if (!created) {
        console_write("  M8 channel self-test: FAIL\n");
        return;
    }

    const char message[] = "hello";
    char received[sizeof(message)] = {0};
    size_t message_size = 0;
    ok &= objtest_check(channel_receive(&table, receive_handle, received,
                                        sizeof(received), &message_size) == CHANNEL_EMPTY,
                        "empty queue reports CHANNEL_EMPTY");
    ok &= objtest_check(channel_send(&table, receive_handle, message,
                                     sizeof(message)) == CHANNEL_DENIED &&
                        channel_receive(&table, send_handle, received,
                                        sizeof(received), NULL) == CHANNEL_DENIED,
                        "endpoint rights restrict send and receive");
    ok &= objtest_check(channel_send(&table, send_handle, message,
                                     sizeof(message)) == CHANNEL_OK &&
                        channel_send(&table, send_handle, message,
                                     sizeof(message)) == CHANNEL_FULL,
                        "bounded queue accepts then rejects when full");

    bool small_buffer = channel_receive(&table, receive_handle, received,
                                        sizeof(message) - 1, &message_size) ==
                            CHANNEL_BUFFER_SMALL && message_size == sizeof(message);
    bool received_message = channel_receive(&table, receive_handle, received,
                                             sizeof(received), &message_size) ==
                                CHANNEL_OK && message_size == sizeof(message) &&
                            str_eq(received, message);
    ok &= objtest_check(small_buffer && received_message,
                        "small buffer preserves queued message and reports size");
    ok &= objtest_check(channel_send(&table, send_handle, message,
                                     CHANNEL_MAX_MESSAGE_SIZE + 1) == CHANNEL_TOO_LARGE,
                        "oversized message is rejected");

    (void)channel_send(&table, send_handle, message, sizeof(message));
    bool closed_sender = handle_close(&table, send_handle);
    bool drained = channel_receive(&table, receive_handle, received,
                                   sizeof(received), NULL) == CHANNEL_OK;
    bool sender_closed = channel_receive(&table, receive_handle, received,
                                         sizeof(received), NULL) == CHANNEL_CLOSED;
    ok &= objtest_check(closed_sender && drained && sender_closed,
                        "closed sender drains queued data then reports closed");
    (void)handle_close(&table, receive_handle);

    send_handle = 0;
    receive_handle = 0;
    bool second_pair = channel_create_pair(&table, 1, &send_handle, &receive_handle);
    bool receiver_close = second_pair && handle_close(&table, receive_handle) &&
        channel_send(&table, send_handle, message, sizeof(message)) == CHANNEL_CLOSED;
    ok &= objtest_check(receiver_close,
                        "closed receiver makes peer send report closed");
    handle_table_close_all(&table);
    ok &= objtest_check(object_count() == before,
                        "channel endpoints release all objects");
    console_write(ok ? "  M8 channel self-test: PASS\n"
                     : "  M8 channel self-test: FAIL\n");
}

typedef struct {
    handle_table_t *table;
    handle_t receive_handle;
    volatile bool started;
    volatile bool finished;
    channel_result_t result;
    size_t size;
    char data[16];
} channel_wait_context_t;

static void channel_wait_worker(void *argument) {
    channel_wait_context_t *context = argument;
    context->started = true;
    context->result = channel_receive_wait(context->table,
        context->receive_handle, context->data, sizeof(context->data),
        &context->size);
    context->finished = true;
}

static bool channel_waiter_parked(channel_wait_context_t *context) {
    for (uint32_t attempt = 0; attempt < 16; attempt++) {
        bool switched = thread_yield();
        if (context->finished) return false;
        if (!switched && context->started) return true;
    }
    return false;
}

static void cmd_chanwaittest(int argc, char **argv) {
    (void)argc;
    (void)argv;
    console_write("Blocking IPC self-test\n");
    uint64_t before = object_count();
    handle_table_t table;
    handle_table_init(&table);
    handle_t sender = 0;
    handle_t receiver = 0;
    bool created = channel_create_pair(&table, 1, &sender, &receiver);
    bool ok = objtest_check(created, "create wait-test channel");
    if (!created) {
        console_write("  M8 blocking IPC self-test: FAIL\n");
        return;
    }

    char byte = 0;
    bool no_runnable = channel_receive_wait(&table, receiver, &byte,
        sizeof(byte), NULL) == CHANNEL_WOULD_BLOCK;
    ok &= objtest_check(no_runnable,
                        "avoid sleeping when no other thread can wake caller");

    channel_wait_context_t context = {
        .table = &table,
        .receive_handle = receiver,
    };
    thread_id_t worker = thread_create(channel_wait_worker, &context);
    bool started = worker != 0 && channel_waiter_parked(&context);
    ok &= objtest_check(started, "receiver thread blocks on an empty channel");
    const char payload[] = "wake";
    bool sent = started && channel_send(&table, sender, payload,
                                        sizeof(payload)) == CHANNEL_OK;
    while (started && !context.finished) {
        if (!thread_yield()) break;
    }
    bool received = sent && context.finished && context.result == CHANNEL_OK &&
        context.size == sizeof(payload) && str_eq(context.data, payload);
    (void)thread_yield();
    ok &= objtest_check(received, "send wakes receiver with the queued message");
    (void)handle_close(&table, sender);
    (void)handle_close(&table, receiver);

    sender = 0;
    receiver = 0;
    bool close_pair = channel_create_pair(&table, 1, &sender, &receiver);
    channel_wait_context_t close_context = {
        .table = &table,
        .receive_handle = receiver,
    };
    worker = close_pair ? thread_create(channel_wait_worker, &close_context) : 0;
    started = worker != 0 && channel_waiter_parked(&close_context);
    bool closed_sender = started && handle_close(&table, sender);
    while (started && !close_context.finished) {
        if (!thread_yield()) break;
    }
    bool close_woke = closed_sender && close_context.finished &&
                      close_context.result == CHANNEL_CLOSED;
    (void)thread_yield();
    ok &= objtest_check(close_woke, "closing final sender wakes blocked receiver");
    (void)handle_close(&table, receiver);
    handle_table_close_all(&table);
    ok &= objtest_check(object_count() == before,
                        "blocking IPC test releases all channel objects");
    console_write(ok ? "  M8 blocking IPC self-test: PASS\n"
                     : "  M8 blocking IPC self-test: FAIL\n");
}

typedef struct {
    handle_table_t *table;
    handle_t receive_handle;
    volatile bool finished;
    channel_result_t result;
} channel_timeout_context_t;

static void channel_timeout_worker(void *argument) {
    channel_timeout_context_t *context = argument;
    uint8_t byte;
    context->result = channel_receive_timeout(context->table,
        context->receive_handle, &byte, sizeof(byte), NULL, 3);
    context->finished = true;
}

static void cmd_chantimeout(int argc, char **argv) {
    (void)argc;
    (void)argv;
    console_write("IPC timeout self-test\n");
    uint64_t before = object_count();
    handle_table_t table;
    handle_table_init(&table);
    handle_t sender = 0;
    handle_t receiver = 0;
    bool created = channel_create_pair(&table, 1, &sender, &receiver);
    bool ok = objtest_check(created, "create timeout-test channel");
    if (!created) {
        console_write("  M8 timeout self-test: FAIL\n");
        return;
    }

    uint64_t start = pit_ticks();
    channel_timeout_context_t context = {
        .table = &table,
        .receive_handle = receiver,
    };
    thread_id_t worker = thread_create(channel_timeout_worker, &context);
    uint64_t guard_deadline = start + 100;
    while (worker != 0 && !context.finished && pit_ticks() < guard_deadline)
        (void)thread_yield();
    bool timed_out = context.finished && context.result == CHANNEL_TIMED_OUT &&
                     pit_ticks() - start >= 3;
    (void)thread_yield();
    ok &= objtest_check(timed_out,
                        "empty receive wakes at its PIT-tick deadline");
    (void)handle_close(&table, sender);
    (void)handle_close(&table, receiver);
    handle_table_close_all(&table);
    ok &= objtest_check(object_count() == before,
                        "timeout test releases all channel objects");
    console_write(ok ? "  M8 timeout self-test: PASS\n"
                     : "  M8 timeout self-test: FAIL\n");
}

static void cmd_capxfer(int argc, char **argv) {
    (void)argc;
    (void)argv;
    console_write("Capability transfer self-test\n");
    uint64_t before = object_count();
    handle_table_t sender_table;
    handle_table_t receiver_table;
    handle_table_init(&sender_table);
    handle_table_init(&receiver_table);
    handle_t send_handle = 0;
    handle_t receive_handle = 0;
    bool created = channel_create_pair_between(&sender_table, &receiver_table,
        1, &send_handle, &receive_handle);
    bool ok = objtest_check(created,
                            "create endpoints in separate handle tables");
    if (!created) {
        console_write("  M8 capability transfer self-test: FAIL\n");
        return;
    }

    object_t *target = object_create(OBJECT_TYPE_EVENT, 0, NULL);
    uint64_t target_id = object_id(target);
    handle_t target_handle = target == NULL ? 0 : handle_create(&sender_table,
        target, OBJECT_RIGHT_READ | OBJECT_RIGHT_WRITE |
                OBJECT_RIGHT_DUPLICATE | OBJECT_RIGHT_INSPECT);
    object_release(target);
    handle_t source_capability = target_handle == 0 ? 0 :
        capability_create(&sender_table, target_handle,
                          OBJECT_RIGHT_READ | OBJECT_RIGHT_WRITE);
    (void)handle_close(&sender_table, target_handle);
    bool have_capability = source_capability != 0;
    ok &= objtest_check(have_capability,
                        "create source capability for transfer");

    channel_result_t enlarged = channel_send_capability(&sender_table,
        send_handle, source_capability, OBJECT_RIGHT_READ | OBJECT_RIGHT_CONTROL);
    bool denied_enlargement = enlarged == CHANNEL_DENIED;
    ok &= objtest_check(denied_enlargement,
                        "reject transfer rights beyond source capability");
    channel_result_t sent = have_capability ? channel_send_capability(
        &sender_table, send_handle, source_capability, OBJECT_RIGHT_READ)
        : CHANNEL_DENIED;
    bool sent_reduced = sent == CHANNEL_OK;
    ok &= objtest_check(sent_reduced, "send a reduced capability to the peer");
    (void)handle_close(&sender_table, source_capability);

    bool preserved = channel_receive(&receiver_table, receive_handle,
        NULL, 0, NULL) == CHANNEL_CAPABILITY_PRESENT;
    handle_t received_capability = 0;
    bool received = channel_receive_capability(&receiver_table, receive_handle,
        &received_capability) == CHANNEL_OK && received_capability != 0;
    object_t *resolved = received ? capability_resolve(&receiver_table,
        received_capability, OBJECT_RIGHT_READ, OBJECT_TYPE_EVENT) : NULL;
    bool read_allowed = resolved != NULL && object_id(resolved) == target_id;
    if (resolved != NULL) object_release(resolved);
    object_t *write_target = received ? capability_resolve(&receiver_table,
        received_capability, OBJECT_RIGHT_WRITE, OBJECT_TYPE_EVENT) : NULL;
    bool write_denied = write_target == NULL;
    if (write_target != NULL) object_release(write_target);
    ok &= objtest_check(preserved && received,
                        "receive capability without consuming it as data");
    ok &= objtest_check(read_allowed && write_denied,
                        "transferred capability retains only reduced authority");

    handle_table_close_all(&sender_table);
    handle_table_close_all(&receiver_table);
    ok &= objtest_check(object_count() == before,
                        "transferred capability and endpoints are released");
    console_write(ok ? "  M8 capability transfer self-test: PASS\n"
                     : "  M8 capability transfer self-test: FAIL\n");
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

    const uint64_t protected_page = 0xFFFF900000001000ULL;
    uintptr_t protected_frame = 0;
    bool page_mapped = false;
    uint64_t mapped_phys = 0;
    bool page_available = !vmm_get_mapping(protected_page, &mapped_phys, NULL);
    if (page_available) {
        protected_frame = pmm_alloc_frame();
        page_mapped = protected_frame != 0 &&
            vmm_map(protected_page, protected_frame, VMM_PRESENT | VMM_WRITE);
    }
    handle_t memory_cap = page_mapped
        ? capability_create_memory_region(&table, protected_page, PAGE_SIZE) : 0;
    handle_t read_only_cap = memory_cap != 0
        ? capability_derive(&table, memory_cap, OBJECT_RIGHT_READ) : 0;
    bool denied_missing = !capability_protect_memory(&table, 0, protected_page,
                                                      VMM_PRESENT);
    bool denied_reduced = read_only_cap != 0 &&
        !capability_protect_memory(&table, read_only_cap, protected_page,
                                   VMM_PRESENT);
    bool denied_wrong_type = !capability_protect_memory(&table, cap,
        protected_page, VMM_PRESENT);
    bool denied_outside = memory_cap != 0 &&
        !capability_protect_memory(&table, memory_cap,
            protected_page + PAGE_SIZE, VMM_PRESENT);
    bool allowed_inside = memory_cap != 0 &&
        capability_protect_memory(&table, memory_cap, protected_page,
                                  VMM_PRESENT | VMM_NX);
    uint64_t protected_flags = 0;
    bool flags_applied = vmm_get_mapping(protected_page, NULL, &protected_flags) &&
        (protected_flags & VMM_NX) != 0 && (protected_flags & VMM_WRITE) == 0;
    ok &= objtest_check(page_mapped && memory_cap != 0 && read_only_cap != 0 &&
                        denied_missing && denied_reduced && denied_wrong_type,
                        "memory protection rejects absent and insufficient capabilities");
    ok &= objtest_check(denied_outside && allowed_inside && flags_applied,
                        "memory capability confines page protection to its range");
    bool memory_revoked = memory_cap != 0 && capability_revoke(&table, memory_cap);
    bool denied_revoked = memory_revoked &&
        !capability_protect_memory(&table, memory_cap, protected_page,
                                   VMM_PRESENT | VMM_WRITE);
    ok &= objtest_check(denied_revoked,
                        "revoked memory capability blocks protected operation");
    if (page_mapped) vmm_unmap(protected_page);
    if (protected_frame != 0) pmm_free_frame(protected_frame);

    handle_table_close_all(&table);
    ok &= objtest_check(object_count() == before && g_objtest_destroyed == 1,
                        "release capabilities and target at final close");
    console_write(ok ? "  M6 capability self-test: PASS\n"
                     : "  M6 capability self-test: FAIL\n");
}

static volatile uint32_t g_threadtest_runs[2];

static void threadtest_worker(void *argument) {
    uintptr_t index = (uintptr_t)argument;
    uint64_t start_tick = pit_ticks();
    while (pit_ticks() == start_tick) {
        __asm__ volatile("pause");
    }
    for (uint32_t i = 0; i < 5; i++) {
        g_threadtest_runs[index]++;
        (void)thread_yield();
    }
}

static void cmd_threadtest(int argc, char **argv) {
    (void)argc;
    (void)argv;
    console_write("Kernel thread self-test\n");
    g_threadtest_runs[0] = 0;
    g_threadtest_runs[1] = 0;
    uint32_t baseline_threads = thread_live_count();
    uint64_t switches_before = thread_switch_count();
    uint64_t preemptions_before = thread_preemption_count();
    thread_id_t first = thread_create(threadtest_worker, (void *)(uintptr_t)0);
    thread_id_t second = thread_create(threadtest_worker, (void *)(uintptr_t)1);
    bool created = first != 0 && second != 0;

    uint64_t deadline = pit_ticks() + 20;
    while (thread_live_count() > baseline_threads && pit_ticks() < deadline) {
        __asm__ volatile("pause");
    }
    uint32_t attempts = 0;
    while (thread_live_count() > baseline_threads && attempts++ < 100) {
        if (!thread_yield()) break;
    }
    (void)thread_yield(); /* reclaim the final exited thread stack */
    bool completed = g_threadtest_runs[0] == 5 && g_threadtest_runs[1] == 5;
    bool cleaned = thread_live_count() == baseline_threads;
    bool switched = thread_switch_count() > switches_before;
    bool preempted = thread_preemption_count() > preemptions_before;
    bool ok = created && completed && cleaned && switched && preempted;
    console_write(created ? "  create two guarded kernel threads: ok\n"
                          : "  create two guarded kernel threads: FAILED\n");
    console_write(completed ? "  round-robin yield and thread return: ok\n"
                            : "  round-robin yield and thread return: FAILED\n");
    console_write(preempted ? "  PIT preempted a non-yielding thread: ok\n"
                            : "  PIT preempted a non-yielding thread: FAILED\n");
    console_write(cleaned ? "  dead thread stacks reaped: ok\n"
                          : "  dead thread stacks reaped: FAILED\n");
    kprintf("  context switches: %llu\n",
            (unsigned long long)(thread_switch_count() - switches_before));
    console_write(ok ? "  M7 thread self-test: PASS\n"
                     : "  M7 thread self-test: FAIL\n");
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
    {"drivers", "list registered device drivers", cmd_drivers},
    {"usb", "show detected USB host controllers", cmd_usb},
    {"neofetch","alias for sysinfo",                    cmd_sysinfo},
    {"about",   "alias for sysinfo",                    cmd_sysinfo},
    {"cpu",     "show CPU vendor and brand",            cmd_cpu},
    {"mem",     "show the physical memory map",         cmd_mem},
    {"pmm",     "physical frame allocator statistics",   cmd_pmm},
    {"heap",    "kernel heap statistics",                cmd_heap},
    {"memtest", "exercise kmalloc/kfree",                cmd_memtest},
    {"vmtest",  "exercise map/unmap/protect",            cmd_vmtest},
    {"asptest", "exercise isolated user address spaces", cmd_asptest},
    {"usermodetest", "enter and return from ring 3", cmd_usermodetest},
    {"drivercaptest", "verify user processes need device capabilities", cmd_drivercaptest},
    {"runapp", "run the boot-provided native .hlx app", cmd_runapp},
    {"stacktest","verify guarded NX kernel stack",       cmd_stacktest},
    {"objects", "show object manager statistics",        cmd_objects},
    {"objtest", "exercise objects and handles",          cmd_objtest},
    {"captest", "exercise capability rights and revocation", cmd_captest},
    {"chantest", "exercise bounded IPC channels", cmd_chantest},
    {"chanwaittest", "exercise blocking receive and wakeups", cmd_chanwaittest},
    {"chantimeout", "exercise timed channel receive", cmd_chantimeout},
    {"capxfer", "exercise channel capability transfer", cmd_capxfer},
    {"threadtest", "exercise kernel threads and round-robin yield", cmd_threadtest},
    {"ticks",   "show the PIT timer tick count",         cmd_ticks},
    {"swint",   "software-trigger the timer vector",      cmd_swint},
    {"pitcount","read the raw PIT counter (diagnostic)",  cmd_pitcount},
    {"fault",   "deliberately trigger a CPU exception",  cmd_fault},
    {"uptime",  "time since boot",                      cmd_uptime},
    {"echo",    "print the arguments back",             cmd_echo},
    {"clear",   "clear the screen",                     cmd_clear},
    {"color",   "turn ANSI colour on/off: color on|off", cmd_color},
    {"version", "show the OS version",                  cmd_version},
    {"halt",    "halt the machine",                     cmd_halt},
    {"reboot",  "restart the machine",                  cmd_reboot},
};

const size_t helix_command_count = sizeof(helix_commands) / sizeof(helix_commands[0]);
