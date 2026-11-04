/* usermode.h — running code in ring 3 (a user process with its own address space). */
#ifndef HELIX_USERMODE_H
#define HELIX_USERMODE_H

#include <stdbool.h>
#include <stdint.h>

/* Start a user program that writes one line and exits with status 7. Returns the
 * exit status, or -1 if the process could not be set up. */
int64_t usermode_run_test(void);

/* True while a user program is running (its exit syscall unwinds to the launcher). */
bool usermode_is_active(void);

/* Called by the exit syscall: unwind back to usermode_run_test. Does not return. */
void usermode_exit(uint64_t status);

#endif /* HELIX_USERMODE_H */
