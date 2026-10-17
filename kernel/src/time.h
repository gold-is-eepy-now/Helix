/* time.h — a monotonic time source for uptime. */
#ifndef HELIX_TIME_H
#define HELIX_TIME_H

#include <stdbool.h>
#include <stdint.h>

/* Calibrate the CPU timestamp counter (TSC) against the PIT. Call once at
 * boot, after the console is up. */
void time_init(void);

/* True if calibration succeeded and uptime is meaningful. */
bool time_available(void);

/* Milliseconds since time_init(). Returns 0 if the clock is unavailable. */
uint64_t time_uptime_ms(void);

/* Read the raw TSC. */
uint64_t time_tsc(void);

#endif /* HELIX_TIME_H */
