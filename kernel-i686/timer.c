/* timer.c — PIT channel 0 at a fixed rate, and the tick counter the IRQ0 handler bumps.
 *
 * The PIT input clock is 1,193,182 Hz. The divisor is that clock divided by the wanted
 * rate, so 100 Hz gives 11932 (about 100.0 Hz; the error is about 0.003%). */
#include "timer.h"

#include "io.h"

#define PIT_CHANNEL0 0x40
#define PIT_COMMAND  0x43
#define PIT_CLOCK_HZ 1193182u

static volatile uint32_t g_ticks;

void pit_init(uint32_t hz) {
    uint32_t divisor = PIT_CLOCK_HZ / hz;
    /* Command byte 0x36: channel 0, low then high byte, mode 3 (square wave), binary. */
    outb(PIT_COMMAND, 0x36);
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));
}

void timer_tick(void) {
    g_ticks++;
}

uint32_t timer_ticks(void) {
    return g_ticks;
}
