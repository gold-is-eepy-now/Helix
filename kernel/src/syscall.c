/* syscall.c — system call interface for user space.
 *
 * M9 milestone: native system-call entry and exit.
 *
 * The kernel exposes a native ABI through the `syscall` instruction.
 * User processes call into the kernel via the syscall gate in the IDT,
 * then the kernel examines the syscall number and arguments (rax, rdi,
 * rsi, rdx, r10, r8, r9) as per the x86_64 ABI and dispatches to the
 * appropriate implementation.
 *
 * All system call handlers share the same signature (7 uint64_t args),
 * matching the x86_64 ABI where the kernel passes all arguments
 * regardless of arity.
 *
 * The system call table is placed in .syscall_table section for the
 * entry stub to locate.
 */

#include <stddef.h>
#include <stdint.h>

#include "console.h"
#include "heap.h"
#include "input.h"
#include "object.h"
#include "panic.h"
#include "syscall.h"
#include "klog.h"
#include "thread.h"
#include "channel.h"
#include "usermode.h"
#include "vmm.h"
#include "ext4.h"
#include "kstring.h"
#include "ufile.h"
#include "usermem.h"

/* Per-process system call table. Currently one system table for kernel
 * initialization; M9 will add per-process tables when user threads exist.
 * Placed in .syscall_table section so the entry stub can locate it. */
__attribute__((section(".syscall_table")))
syscall_handler_t syscall_table[64u] = {0};

/* Forward declarations — all use the full 7-argument ABI signature. */
static uint64_t handle_test(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
static uint64_t handle_alloc(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
static uint64_t handle_free(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
static uint64_t handle_map(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
static uint64_t handle_unmap(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
static uint64_t handle_thread_create(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
static uint64_t handle_thread_exit(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
static uint64_t handle_thread_yield(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
static uint64_t handle_channel_send(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
static uint64_t handle_channel_recv(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
static uint64_t handle_handle_create(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
static uint64_t handle_handle_close(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
static uint64_t handle_handle_duplicate(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
static uint64_t handle_handle_get(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
static uint64_t handle_object_lookup(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
static uint64_t handle_object_release(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
static uint64_t handle_object_count(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
static uint64_t handle_object_create(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
static uint64_t handle_object_retain(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
static uint64_t handle_read(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
static uint64_t handle_write(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
static uint64_t handle_read_input(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
static uint64_t handle_exit(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
static uint64_t handle_brk(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
static uint64_t handle_execve(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
static uint64_t handle_debug_write(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
static uint64_t handle_open(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
static uint64_t handle_fd_close(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
static uint64_t handle_lseek(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);

void syscall_init(void) {
    for (size_t i = 0; i < 64u; i++) {
        syscall_table[i] = NULL;
    }

    syscall_table[SYSCALL_TEST]          = handle_test;
    syscall_table[SYSCALL_ALLOC]         = handle_alloc;
    syscall_table[SYSCALL_FREE]          = handle_free;
    syscall_table[SYSCALL_MAP]           = handle_map;
    syscall_table[SYSCALL_UNMAP]         = handle_unmap;
    syscall_table[SYSCALL_THREAD_CREATE] = handle_thread_create;
    syscall_table[SYSCALL_THREAD_EXIT]   = handle_thread_exit;
    syscall_table[SYSCALL_THREAD_YIELD]  = handle_thread_yield;
    syscall_table[SYSCALL_CHANNEL_SEND]  = handle_channel_send;
    syscall_table[SYSCALL_CHANNEL_RECV]  = handle_channel_recv;
    syscall_table[SYSCALL_HANDLE_CREATE] = handle_handle_create;
    syscall_table[SYSCALL_HANDLE_CLOSE]  = handle_handle_close;
    syscall_table[SYSCALL_HANDLE_DUPLICATE] = handle_handle_duplicate;
    syscall_table[SYSCALL_HANDLE_GET]    = handle_handle_get;
    syscall_table[SYSCALL_OBJECT_LOOKUP] = handle_object_lookup;
    syscall_table[SYSCALL_OBJECT_RELEASE] = handle_object_release;
    syscall_table[SYSCALL_OBJECT_COUNT]  = handle_object_count;
    syscall_table[SYSCALL_OBJECT_CREATE] = handle_object_create;
    syscall_table[SYSCALL_OBJECT_RETAIN] = handle_object_retain;
    syscall_table[SYSCALL_READ]          = handle_read;
    syscall_table[SYSCALL_WRITE]         = handle_write;
    syscall_table[SYSCALL_READ_INPUT]    = handle_read_input;
    syscall_table[SYSCALL_EXIT]          = handle_exit;
    syscall_table[SYSCALL_BRK]           = handle_brk;
    syscall_table[SYSCALL_EXECVE]        = handle_execve;
    syscall_table[SYSCALL_DEBUG_WRITE]   = handle_debug_write;
    syscall_table[SYSCALL_OPEN]          = handle_open;
    syscall_table[SYSCALL_CLOSE]         = handle_fd_close;
    syscall_table[SYSCALL_LSEEK]         = handle_lseek;
    syscall_table[SYSCALL_STAT]          = sys_stat;
    syscall_table[SYSCALL_LISTDIR]       = sys_listdir;
    syscall_table[SYSCALL_MKDIR]         = sys_mkdir;
    syscall_table[SYSCALL_UNLINK]        = sys_unlink;
    syscall_table[SYSCALL_RMDIR]         = sys_rmdir;
    syscall_table[SYSCALL_TRUNCATE]      = sys_truncate;
}

/* -- Handler implementations ------------------------------------------ */

static uint64_t handle_test(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    kprintf("[syscall] test arg1=%llu\n", a1);
    return 0;
}

static uint64_t handle_alloc(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    void *ptr = kmalloc(a1);
    if (ptr == NULL) return UINT64_MAX;
    kprintf("[syscall] alloc %llu bytes at %p\n", a1, ptr);
    return (uint64_t)(uintptr_t)ptr;
}

static uint64_t handle_free(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    kfree((void *)(uintptr_t)a1);
    kprintf("[syscall] free %p\n", (void *)(uintptr_t)a1);
    return 0;
}

static uint64_t handle_map(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; (void)a3;
    if (!vmm_map(a1, a2, VMM_PRESENT | VMM_WRITE)) {
        return UINT64_MAX;
    }
    kprintf("[syscall] map %p -> %p\n", a1, a2);
    return 0;
}

static uint64_t handle_unmap(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    if (!vmm_unmap(a1)) {
        return UINT64_MAX;
    }
    kprintf("[syscall] unmap %p\n", a1);
    return 0;
}

static uint64_t handle_thread_create(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    thread_t *t = thread_create((void (*)(void))(uintptr_t)a1);
    if (t == NULL) return UINT64_MAX;
    kprintf("[syscall] thread created\n");
    return (uint64_t)(uintptr_t)t;
}

static uint64_t handle_thread_exit(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    kprintf("[syscall] thread exiting\n");
    thread_exit_current();
    return 0;
}

static uint64_t handle_thread_yield(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    thread_yield();
    return 0;
}

static uint64_t handle_channel_send(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    channel_send((channel_t *)(uintptr_t)a1, a2);
    return 0;
}

static uint64_t handle_channel_recv(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    if (!channel_recv((channel_t *)(uintptr_t)a1, (uint64_t *)a2)) {
        return UINT64_MAX;
    }
    return 0;
}

static uint64_t handle_handle_create(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    handle_t h = handle_create(object_kernel_handles(),
                               object_lookup(a1), (uint32_t)a2);
    if (h == 0) return UINT64_MAX;
    return h;
}

static uint64_t handle_handle_close(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    if (!handle_close(object_kernel_handles(), (handle_t)a1)) {
        return UINT64_MAX;
    }
    return 0;
}

static uint64_t handle_handle_duplicate(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    handle_t h = handle_duplicate(object_kernel_handles(),
                                  (handle_t)a1, (uint32_t)a2);
    if (h == 0) return UINT64_MAX;
    return h;
}

static uint64_t handle_handle_get(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    object_t *obj = handle_get(object_kernel_handles(),
                               (handle_t)a1, (uint32_t)a2,
                               OBJECT_TYPE_NONE);
    if (obj == NULL) return UINT64_MAX;
    object_release(obj);
    return a1;
}

static uint64_t handle_object_lookup(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    object_t *obj = object_lookup(a1);
    if (obj == NULL) return UINT64_MAX;
    object_release(obj);
    return a1;
}

static uint64_t handle_object_release(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    object_release((object_t *)(uintptr_t)a1);
    return 0;
}

static uint64_t handle_object_count(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    return object_count();
}

static uint64_t handle_object_create(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    object_t *obj = object_create((object_type_t)a1, a2, NULL);
    if (obj == NULL) return UINT64_MAX;
    object_release(obj);
    return object_id(obj);
}

static uint64_t handle_object_retain(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    if (!object_retain((object_t *)(uintptr_t)a1)) {
        return UINT64_MAX;
    }
    return 0;
}

/* Largest piece moved between user memory and a kernel buffer at a time. */
#define IO_CHUNK 512u

/* read(fd, buf, count): fd 0 is the console and is read with SYS_READ_INPUT, so
 * it returns -1 here. Other descriptors read from their file. Returns the number
 * of bytes read (0 at end of file), or -1. */
static uint64_t handle_read(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a4; (void)a5; (void)a6; (void)a7;
    if (a1 < 3) return UINT64_MAX;
    if (!usermem_valid(a2, a3, true)) return UINT64_MAX;
    uint8_t kbuf[IO_CHUNK];
    uint64_t total = 0;
    while (total < a3) {
        uint64_t want = a3 - total < IO_CHUNK ? a3 - total : IO_CHUNK;
        int64_t got = ufile_read((int64_t)a1, kbuf, want);
        if (got < 0) return total > 0 ? total : UINT64_MAX;
        if (got == 0) break;
        usermem_copy_out(a2 + total, kbuf, (uint64_t)got);
        total += (uint64_t)got;
        if ((uint64_t)got < want) break;
    }
    return total;
}

/* write(fd, buf, count): fd 1 and 2 go to the console; other descriptors write
 * to their file. Returns the number of bytes written, or -1. */
static uint64_t handle_write(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a4; (void)a5; (void)a6; (void)a7;
    if (!usermem_valid(a2, a3, false)) return UINT64_MAX;
    uint8_t kbuf[IO_CHUNK];
    uint64_t total = 0;
    while (total < a3) {
        uint64_t want = a3 - total < IO_CHUNK ? a3 - total : IO_CHUNK;
        usermem_copy_in(kbuf, a2 + total, want);
        if (a1 == 1 || a1 == 2) {
            for (uint64_t i = 0; i < want; i++) console_putc((char)kbuf[i]);
            total += want;
            continue;
        }
        int64_t put = ufile_write((int64_t)a1, kbuf, want);
        if (put < 0) return total > 0 ? total : UINT64_MAX;
        total += (uint64_t)put;
        if ((uint64_t)put < want) break;
    }
    return total;
}

/* open(path, flags): path is a NUL-terminated absolute path in user memory.
 * Returns a descriptor, or -1. */
static uint64_t handle_open(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    char path[256];
    if (!usermem_copy_str(path, sizeof(path), a1)) return UINT64_MAX;
    int64_t fd = ufile_open(path, a2);
    return fd < 0 ? UINT64_MAX : (uint64_t)fd;
}

static uint64_t handle_fd_close(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    return ufile_close((int64_t)a1) < 0 ? UINT64_MAX : 0;
}

/* lseek(fd, offset, whence): offset is signed. Returns the new position. */
static uint64_t handle_lseek(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a4; (void)a5; (void)a6; (void)a7;
    int64_t pos = ufile_seek((int64_t)a1, (int64_t)a2, (int)a3);
    return pos < 0 ? UINT64_MAX : (uint64_t)pos;
}

/* read_input(fd): return the next console character, or (uint64_t)-1 if none.
 * It never blocks: a ring-3 program polls it. Interrupts are off in ring 3, so
 * the kernel reads serial and the keyboard directly here. Arrow keys come back
 * as SHELL_KEY_UP / SHELL_KEY_DOWN. Only fd 0 (the console) is supported. */
static uint64_t handle_read_input(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    if (a1 != 0) {
        return (uint64_t)-1;
    }
    return (uint64_t)(int64_t)input_getchar();
}

/* Lowest address above the user half: user pointers must stay below it. */
#define USER_VA_LIMIT 0x0000800000000000ULL

static uint64_t handle_debug_write(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    /* a1 = user buffer, a2 = length. Writes straight to the console; user
     * processes will get a console handle once handles reach them. */
    if (a2 > 256u || a1 >= USER_VA_LIMIT || a2 > USER_VA_LIMIT - a1) return UINT64_MAX;
    const char *p = (const char *)(uintptr_t)a1;
    for (uint64_t i = 0; i < a2; i++) console_putc(p[i]);
    return a2;
}

static uint64_t handle_exit(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    if (usermode_is_active()) {
        usermode_exit(a1);   /* unwinds to usermode_run_test; does not return */
    }
    kprintf("[syscall] exit: status=%llu\n", a1);
    thread_exit_current();
    return 0;
}

/* brk(addr): move the process's program break. See usermode_brk. */
static uint64_t handle_brk(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    return usermode_brk(a1);
}

/* execve(path, argv): run the program at the absolute path `path` as a child
 * process and wait for it. This kernel has no fork, so execve does not replace
 * the caller. It starts a child with the given argv and returns the child's exit
 * status, or -1 if the child could not run. argv is a NULL-terminated array of
 * user pointers; a NULL argv gives the child just its path as argv[0]. */
/* SYS_EXECVE(path, argv, envp): run a program from the root and wait for it.
 * argv and envp are NULL-terminated arrays of user pointers. A NULL envp means
 * the child inherits the caller's environment. */
static uint64_t handle_execve(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a4; (void)a5; (void)a6; (void)a7;
    char path[256];
    if (!usermem_copy_str(path, sizeof(path), a1)) return UINT64_MAX;

    /* One kernel buffer holds the copied argv strings, then the environment. */
    char *store = kmalloc(USERMODE_ARG_BYTES + USERMODE_ENV_BYTES);
    if (store == NULL) return UINT64_MAX;
    char *env_store = store + USERMODE_ARG_BYTES;
    const char *argv_k[USERMODE_MAX_ARGS];
    const char *envp_k[USERMODE_MAX_ENV];
    int argc = 0;
    int envc = -1;
    size_t used = 0;
    size_t env_used = 0;

    if (a2 != 0) {
        for (;;) {
            uint64_t p = 0;
            if (argc == (int)USERMODE_MAX_ARGS) goto fail;
            if (!usermem_copy_in(&p, a2 + (uint64_t)argc * 8u, 8)) goto fail;
            if (p == 0) break;
            if (!usermem_copy_str(store + used, USERMODE_ARG_BYTES - used, p)) goto fail;
            argv_k[argc++] = store + used;
            used += strlen(store + used) + 1u;
        }
    }
    if (argc == 0) {
        size_t n = strlen(path) + 1u;
        if (n > USERMODE_ARG_BYTES) goto fail;
        memcpy(store, path, n);
        argv_k[argc++] = store;
    }

    if (a3 != 0) {
        envc = 0;
        for (;;) {
            uint64_t p = 0;
            if (envc == (int)USERMODE_MAX_ENV) goto fail;
            if (!usermem_copy_in(&p, a3 + (uint64_t)envc * 8u, 8)) goto fail;
            if (p == 0) break;
            if (!usermem_copy_str(env_store + env_used, USERMODE_ENV_BYTES - env_used, p)) goto fail;
            envp_k[envc++] = env_store + env_used;
            env_used += strlen(env_store + env_used) + 1u;
        }
    }

    int64_t status = usermode_spawn_env(path, argc, argv_k, envc, envp_k);
    kfree(store);
    return (uint64_t)status;

fail:
    kfree(store);
    return UINT64_MAX;
}

void syscall_dispatch(registers_t *regs) {
    uint64_t syscall_num = regs->rax;
    
    if (syscall_num >= 64u) {
        regs->rax = UINT64_MAX; /* invalid syscall number */
        return;
    }
    
    syscall_handler_t handler = syscall_table[syscall_num];
    /* A keystroke poll (read_input with no buffer) returns -1 on most calls; logging
     * each one would fill the ring with noise and push out the events before a crash. */
    bool poll = (syscall_num == SYSCALL_READ_INPUT && regs->rdi == 0);
    if (!poll) {
        klog_event(KLOG_SYSCALL, "nr=%llu a1=%p a2=%p a3=%p", (unsigned long long)syscall_num,
                   (uintptr_t)regs->rdi, (uintptr_t)regs->rsi, (uintptr_t)regs->rdx);
    }
    if (handler == NULL) {
        regs->rax = UINT64_MAX; /* unimplmented syscall */
        klog_event(KLOG_SYSCALL, "nr=%llu not implemented", (unsigned long long)syscall_num);
        return;
    }
    
    /* Call the handler with the 7-argument ABI signature. The user passed
     * arguments in rdi, rsi, rdx, r10, r8, r9; we forward them in that order.
     * The seventh argument is unused by all handlers. */
    regs->rax = handler(regs->rdi, regs->rsi, regs->rdx,
                        regs->r10, regs->r8, regs->r9, 0);
    if (!poll || regs->rax != UINT64_MAX) {
        klog_event(KLOG_SYSCALL, "nr=%llu returned %p", (unsigned long long)syscall_num,
                   (uintptr_t)regs->rax);
    }
}