/* usermode.h — running code in ring 3 (a user process with its own address space). */
#ifndef HELIX_USERMODE_H
#define HELIX_USERMODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Start a user program that writes one line and exits with status 7. Returns the
 * exit status, or -1 if the process could not be set up. */
int64_t usermode_run_test(void);

/* Run a static ELF64 executable in a new process. Returns its exit status, -1 if
 * the process could not be set up, or -2 if the image is not a loadable ET_EXEC
 * x86_64 file. */
int64_t usermode_run_elf(const uint8_t *img, size_t len);

/* Run the userland shell embedded in the kernel (see userspace/shell). */
int64_t usermode_run_user_shell(void);

/* Run the system init process embedded in the kernel (PID 1). */
int64_t usermode_run_init(void);

/* True while a user program is running (its exit syscall unwinds to the launcher). */
bool usermode_is_active(void);

/* Called by the exit syscall: unwind back to usermode_run_test. Does not return. */
void usermode_exit(uint64_t status);

#endif /* HELIX_USERMODE_H */
