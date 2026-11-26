/* intr_test.h — bring-up of the interrupt subsystem and its self-tests (stage I3a). */
#ifndef HELIX_I686_INTR_TEST_H
#define HELIX_I686_INTR_TEST_H

#include <stdbool.h>
#include <stdint.h>

/* Load the IDT, remap the PIC, start the PIT at `hz`. Interrupts stay disabled. */
void interrupts_init(uint32_t hz);

/* int3 enters the breakpoint handler and returns: the handler must run exactly once. */
bool breakpoint_selftest(void);

/* Enable interrupts until 10 ticks have passed, then disable them again. A timeout bound
 * makes a missing timer fail the test instead of hanging the boot. */
bool timer_selftest(void);

#endif /* HELIX_I686_INTR_TEST_H */
