#ifndef HELIX_LINUX_RUNTIME_H
#define HELIX_LINUX_RUNTIME_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -- Constants (must match kernel/src/syscall.h) -- */

/* Native Helix syscall numbers. These are defined in the kernel's
 * syscall.h header and must be kept in sync. */
#define SYSCALL_READ        20
#define SYSCALL_WRITE       21
#define SYSCALL_EXIT        23
#define SYSCALL_BRK         24
#define SYSCALL_HANDLE_CREATE  37
#define SYSCALL_HANDLE_CLOSE   38
#define SYSCALL_HANDLE_DUPLICATE 39
#define SYSCALL_HANDLE_GET  40
#define SYSCALL_OBJECT_LOOKUP  41
#define SYSCALL_OBJECT_RELEASE 42
#define SYSCALL_OBJECT_COUNT   43
#define SYSCALL_OBJECT_CREATE  44

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