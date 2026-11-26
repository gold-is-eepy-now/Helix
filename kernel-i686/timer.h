/* timer.h — the 8254 PIT (programmable interval timer), used as the periodic tick. */
#ifndef HELIX_I686_TIMER_H
#define HELIX_I686_TIMER_H

#include <stdint.h>

/* Program channel 0 as a square-wave generator at about `hz` ticks per second. */
void pit_init(uint32_t hz);

/* Called from the timer interrupt handler once per tick. */
void timer_tick(void);

/* Ticks since pit_init(). Reads are plain loads: the handler is the only writer. */
uint32_t timer_ticks(void);

#endif /* HELIX_I686_TIMER_H */
