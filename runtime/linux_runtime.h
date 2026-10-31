#ifndef HELIX_LINUX_RUNTIME_H
#define HELIX_LINUX_RUNTIME_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -- Constants (must match the enum in kernel/src/syscall.h) --
 *
 * The kernel dispatches int $0x80 by indexing syscall_table[rax] with these
 * numbers; a wrong value silently hits a NULL slot. Keep in sync manually —
 * runtime_test.c pins the critical ones to literals. */
#define SYSCALL_TEST             1
#define SYSCALL_ALLOC            2
#define SYSCALL_FREE             3
#define SYSCALL_MAP              4
#define SYSCALL_UNMAP            5
#define SYSCALL_THREAD_CREATE    6
#define SYSCALL_THREAD_EXIT      7
#define SYSCALL_THREAD_YIELD     8
#define SYSCALL_CHANNEL_SEND     9
#define SYSCALL_CHANNEL_RECV    10
#define SYSCALL_HANDLE_CREATE   11
#define SYSCALL_HANDLE_CLOSE    12
#define SYSCALL_HANDLE_DUPLICATE 13
#define SYSCALL_HANDLE_GET      14
#define SYSCALL_OBJECT_LOOKUP   15
#define SYSCALL_OBJECT_RELEASE  16
#define SYSCALL_OBJECT_COUNT    17
#define SYSCALL_OBJECT_CREATE   18
#define SYSCALL_OBJECT_RETAIN   19
#define SYSCALL_READ            20
#define SYSCALL_WRITE           21
#define SYSCALL_READ_INPUT      22
#define SYSCALL_EXIT            23
#define SYSCALL_BRK             24
#define SYSCALL_EXECVE          25
#define SYSCALL_NATIVE_MAX      26  /* one past the last native number */

/* Handle type for the object manager. Defined in kernel/src/object.h */
typedef uint32_t handle_t;

/* -- Runtime interface -- */

/* Initialize the Linux runtime and start the main loop. This is the
 * main entry point for the runtime process. */
void linux_runtime_start(void);

/* Initialize the FD-to-handle table. Called automatically on first use. */
void linux_fd_table_init(void);

/* Map a Linux file descriptor to a Helix handle. Returns 0 if invalid. */
handle_t linux_fd_to_handle(int fd);

/* Set the mapping from a Linux fd to a Helix handle. Returns 0 on success,
 * -1 on failure (invalid fd). */
int linux_set_fd_handle(int fd, handle_t handle);

/* Translate a Linux syscall number to the native Helix syscall number.
 * Returns the native index, or -1 if unmapped. */
int linux_translate_number(int linux_num);

#ifdef __cplusplus
}
#endif

#endif /* HELIX_LINUX_RUNTIME_H */