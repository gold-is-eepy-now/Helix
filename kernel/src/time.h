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
/* TSC ticks per millisecond from calibration; 0 until calibrated. */
uint64_t time_tsc_per_ms(void);

/* Milliseconds since time_init(). Returns 0 if the clock is unavailable. */
uint64_t time_uptime_ms(void);

/* Read the raw TSC. */
uint64_t time_tsc(void);

/* Set the wall-clock epoch (seconds since 1970-01-01 UTC) observed at boot,
 * so date/time commands can report a real calendar instead of a constant. */
void time_set_boot_epoch(uint64_t epoch_seconds);

/* Best-effort wall-clock epoch from the CMOS RTC. Returns 0 if unavailable. */
uint64_t time_rtc_epoch(void);

/* Seconds since the Unix epoch now, combining the boot epoch with elapsed
 * uptime. Returns 0 if no wall clock is available. */
uint64_t time_epoch(void);

/* Split the current wall clock into UTC components. Safe even with no clock. */
void time_now_utc(int *year, int *month, int *day,
                 int *hour, int *minute, int *second,
                 int *weekday);

#endif /* HELIX_TIME_H */
