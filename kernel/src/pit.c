/* pit.c — periodic timer via PIT channel 0.
 *
 * The PIT runs at a fixed 1.193182 MHz. Writing a 16-bit divisor to channel 0
 * makes it raise IRQ0 every (divisor / 1.193182 MHz) seconds. We use mode 3
 * (square wave), which is the usual choice for a periodic system tick.
 *
 * This is separate from the one-shot use of channel 2 in time.c, which
 * calibrates the TSC.
 */
#include "pit.h"

#include "io.h"

#define PIT_CHANNEL0 0x40u
#define PIT_COMMAND  0x43u
#define PIT_BASE_HZ  1193182u

static volatile uint64_t g_ticks;
static uint32_t g_hz;

void pit_init(uint32_t hz) {
    g_hz = hz;
    g_ticks = 0;

    uint32_t divisor = PIT_BASE_HZ / hz;
    if (divisor == 0) {
        divisor = 1;
    }
    if (divisor > 0xFFFF) {
        divisor = 0xFFFF;
    }

    outb(PIT_COMMAND, 0x36); /* channel 0, lo/hi byte, mode 3, binary */
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));
}

void pit_tick(void) {
    g_ticks++;
}

uint64_t pit_ticks(void) {
    return g_ticks;
}

uint32_t pit_hz(void) {
    return g_hz;
}

uint64_t pit_uptime_ms(void) {
    if (g_hz == 0) {
        return 0;
    }
    return (g_ticks * 1000u) / g_hz;
}
