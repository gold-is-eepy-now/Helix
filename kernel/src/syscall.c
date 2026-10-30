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
#include "object.h"
#include "panic.h"
#include "syscall.h"
#include "thread.h"
#include "channel.h"
#include "vmm.h"

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

static uint64_t handle_read(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a1; (void)a2; (void)a3;
    (void)a4; (void)a5; (void)a6; (void)a7;
    kprintf("[syscall] read: stub\n");
    return 0;
}

static uint64_t handle_write(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a1; (void)a2; (void)a3;
    (void)a4; (void)a5; (void)a6; (void)a7;
    kprintf("[syscall] write: stub\n");
    return 0;
}

static uint64_t handle_read_input(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a1; (void)a2;
    (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    kprintf("[syscall] read_input: stub\n");
    return 0;
}

static uint64_t handle_exit(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    kprintf("[syscall] exit: status=%llu\n", a1);
    thread_exit_current();
    return 0;
}

static uint64_t handle_brk(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    kprintf("[syscall] brk\n");
    return 0;
}

static uint64_t handle_execve(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    kprintf("[syscall] execve\n");
    return 0;
}

void syscall_dispatch(registers_t *regs) {
    uint64_t syscall_num = regs->rax;
    
    if (syscall_num >= 64u) {
        regs->rax = UINT64_MAX; /* invalid syscall number */
        return;
    }
    
    syscall_handler_t handler = syscall_table[syscall_num];
    if (handler == NULL) {
        regs->rax = UINT64_MAX; /* unimplmented syscall */
        return;
    }
    
    /* Call the handler with the 7-argument ABI signature. The user passed
     * arguments in rdi, rsi, rdx, r10, r8, r9; we forward them in that order.
     * The seventh argument is unused by all handlers. */
    regs->rax = handler(regs->rdi, regs->rsi, regs->rdx,
                        regs->r10, regs->r8, regs->r9, 0);
}