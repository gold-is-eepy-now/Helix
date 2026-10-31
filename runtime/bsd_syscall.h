#ifndef HELIX_BSD_SYSCALL_H
#define HELIX_BSD_SYSCALL_H

/* BSD syscall translation layer for Helix OS.
 *
 * Maps BSD (FreeBSD/NetBSD/OpenBSD) system call numbers to Helix native syscalls.
 * BSD uses different syscall numbers than Linux on x86_64.
 *
 * This header depends on handle_t being defined by linux_runtime.h.
 */

#include <stdint.h>
#include <stddef.h>
#include "linux_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Translate a single BSD system call.
 *
 * `number` is the BSD syscall number (e.g., 3 = read, 4 = write, 5 = open).
 * `arg1`-`arg7` are the BSD syscall arguments as passed in the x86_64 ABI.
 * Returns the result (return value) of the translated syscall, or (uint64_t)-1
 * on error with errno set appropriately.
 */
uint64_t bsd_translate_syscall(int number, uint64_t arg1, uint64_t arg2,
                                uint64_t arg3, uint64_t arg4,
                                uint64_t arg5, uint64_t arg6, uint64_t arg7);

/* Map a BSD file descriptor to a Helix object handle.
 * Uses the same FD table as Linux runtime.
 */
handle_t bsd_fd_to_handle(int fd);

/* Set the mapping from a BSD fd to a Helix handle. */
int bsd_set_fd_handle(int fd, handle_t handle);

/* Translate a BSD syscall number to its native Helix equivalent.
 * Returns the native syscall index, or -1 if unmapped. */
int bsd_map_to_native(int bsd_num);

/* Get the current error number (errno) for the calling BSD process. */
int bsd_errno(void);

#ifdef __cplusplus
}
#endif

#endif /* HELIX_BSD_SYSCALL_H */