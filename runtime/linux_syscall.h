#ifndef HELIX_LINUX_SYSCALL_H
#define HELIX_LINUX_SYSCALL_H

/* Linux/BSD syscall translation layer for Helix OS.
 *
 * Maps Linux system call numbers and arguments to Helix native syscalls.
 * The runtime intercepts Linux `int $0x80` (or `syscall`) invocations and
 * calls into this layer to translate each syscall into its native equivalent.
 *
 * This header depends on handle_t being defined by linux_runtime.h.
 */

#include <stdint.h>
#include <stddef.h>
#include "linux_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Translate a single Linux system call.
 *
 * `number` is the Linux syscall number (e.g., 1 = write, 2 = open, etc.).
 * `arg1`-`arg7` are the Linux syscall arguments as passed in the x86_64 ABI.
 * Returns the result (return value) of the translated syscall, or (uint64_t)-1
 * on error with errno set appropriately.
 */
uint64_t linux_translate_syscall(int number, uint64_t arg1, uint64_t arg2,
                                   uint64_t arg3, uint64_t arg4,
                                   uint64_t arg5, uint64_t arg6, uint64_t arg7);

/* Map a Linux file descriptor to a Helix object handle.
 *
 * The Linux runtime maintains an internal table mapping Linux FDs to Helix
 * file handles. This function looks up the handle for a given Linux FD.
 * Returns the handle_t on success, or 0 on failure (invalid FD).
 * Declared in linux_runtime.h.
 */

/* Get the current error number (errno) for the calling Linux process. */
int linux_errno(void);

/* Invoke a native Helix syscall directly via int $0x80.
 * `native_num` is the Helix syscall index (e.g., SYSCALL_READ).
 * Used by both Linux and BSD translation layers. */
uint64_t native_syscall_invoke(int native_num, uint64_t arg1, uint64_t arg2,
                                uint64_t arg3, uint64_t arg4,
                                uint64_t arg5, uint64_t arg6, uint64_t arg7);

#ifdef __cplusplus
}
#endif

#endif /* HELIX_LINUX_SYSCALL_H */