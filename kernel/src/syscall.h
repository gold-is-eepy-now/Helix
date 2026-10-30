/* syscall.h — system call interface for user space.
 *
 * M9 milestone: native system-call entry and exit.
 *
 * The kernel exposes a native ABI through the `syscall` instruction.
 * User processes call into the kernel via the syscall gate in the IDT,
 * then the kernel examines the syscall number and arguments (rax, rdi,
 * rsi, rdx, r10, r8, r9) as per the x86_64 ABI and dispatches to the
 * appropriate implementation.
 *
 * The system call table is placed in a special section (.syscall_table)
 * so that the entry stub can locate it easily. The entry stub validates that
 * the caller is in ring 3 (user mode) before dispatching.
 */
#ifndef HELIX_SYSCALL_H
#define HELIX_SYSCALL_H

#include <stdint.h>
#include "idt.h"

#ifdef __cplusplus
extern "C" {
#endif

/* System call numbers for the native Helix ABI. */
enum {
    SYSCALL_TEST = 1,
    SYSCALL_ALLOC,
    SYSCALL_FREE,
    SYSCALL_MAP,
    SYSCALL_UNMAP,
    SYSCALL_THREAD_CREATE,
    SYSCALL_THREAD_EXIT,
    SYSCALL_THREAD_YIELD,
    SYSCALL_CHANNEL_SEND,
    SYSCALL_CHANNEL_RECV,
    SYSCALL_HANDLE_CREATE,
    SYSCALL_HANDLE_CLOSE,
    SYSCALL_HANDLE_DUPLICATE,
    SYSCALL_HANDLE_GET,
    SYSCALL_OBJECT_LOOKUP,
    SYSCALL_OBJECT_RELEASE,
    SYSCALL_OBJECT_COUNT,
    SYSCALL_OBJECT_CREATE,
    SYSCALL_OBJECT_RETAIN,
    SYSCALL_READ,
    SYSCALL_WRITE,
    SYSCALL_READ_INPUT,
    SYSCALL_EXIT,
    SYSCALL_BRK,
    SYSCALL_EXECVE,
};

/* System call handler signature. All syscalls use a uniform 7-argument
 * interface matching the x86_64 ABI: rax = syscall number, then rdi,
 * rsi, rdx, r10, r8, r9 for the first six arguments. The seventh arg
 * is passed in the kernel stack slot or via a struct on stack. */
typedef uint64_t (*syscall_handler_t)(uint64_t arg1, uint64_t arg2,
                                    uint64_t arg3, uint64_t arg4,
                                    uint64_t arg5, uint64_t arg6,
                                    uint64_t arg7);

void syscall_init(void);
void syscall_dispatch(registers_t *regs);

#ifdef __cplusplus
}
#endif

#endif /* HELIX_SYSCALL_H */