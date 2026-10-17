/* pit.h — the 8253/8254 Programmable Interval Timer as a periodic tick. */
#ifndef HELIX_PIT_H
#define HELIX_PIT_H

#include <stdint.h>

/* Program PIT channel 0 to fire IRQ0 at `hz` Hz. */
void pit_init(uint32_t hz);

/* Called from the IRQ0 handler. */
void pit_tick(void);

/* Ticks since pit_init(). */
uint64_t pit_ticks(void);

/* Configured frequency in Hz. */
uint32_t pit_hz(void);

/* Milliseconds derived from ticks (coarser than the TSC, but independent). */
uint64_t pit_uptime_ms(void);

#endif /* HELIX_PIT_H */
