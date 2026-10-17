/* time.c — uptime from the CPU timestamp counter, calibrated with the PIT.
 *
 * The TSC is a 64-bit counter inside the CPU that ticks at a constant rate.
 * It is perfect for measuring elapsed time, but its rate (the frequency) is
 * not fixed by any standard. So at boot we measure it once against the
 * Programmable Interval Timer (PIT), whose frequency *is* known: 1.193182 MHz.
 *
 * We use PIT channel 2 in "one-shot" mode: program it for a known number of
 * ticks, start it, and measure how many TSC ticks elapse. That gives us
 * TSC ticks per millisecond.
 *
 * No interrupts are involved; we simply poll the PIT's output bit.
 */
#include "time.h"

#include "io.h"

#define PIT_HZ 1193182u

static uint64_t g_boot_tsc;
static uint64_t g_tsc_per_ms; /* 0 until calibrated */

static uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

uint64_t time_tsc(void) {
    return rdtsc();
}

/* Start PIT channel 2 as a one-shot counter for `count` ticks. */
static void pit2_oneshot(uint16_t count) {
    uint8_t port61 = inb(0x61);
    outb(0x61, (uint8_t)(port61 & ~0x01)); /* gate low: hold the counter   */
    outb(0x43, 0xB0);                      /* channel 2, mode 0, lo/hi byte */
    outb(0x42, (uint8_t)(count & 0xFF));
    outb(0x42, (uint8_t)(count >> 8));
    /* Gate high starts counting; also make sure the speaker stays off. */
    outb(0x61, (uint8_t)((port61 & ~0x02) | 0x01));
}

/* Wait until channel 2's output goes high (count reached zero). */
static void pit2_wait(void) {
    while ((inb(0x61) & 0x20) == 0) {
        /* spin */
    }
}

void time_init(void) {
    g_boot_tsc = rdtsc();
    g_tsc_per_ms = 0;

    /* Aim for ~50 ms: 1193182 Hz * 0.05 s = 59659 ticks. */
    const uint32_t window_ms = 50;
    const uint16_t count = (uint16_t)((PIT_HZ * window_ms) / 1000u);

    pit2_oneshot(count);
    uint64_t start = rdtsc();
    pit2_wait();
    uint64_t end = rdtsc();

    uint64_t delta = end - start;
    if (delta > 0) {
        g_tsc_per_ms = delta / window_ms;
    }
}

bool time_available(void) {
    return g_tsc_per_ms != 0;
}

uint64_t time_uptime_ms(void) {
    if (g_tsc_per_ms == 0) {
        return 0;
    }
    return (rdtsc() - g_boot_tsc) / g_tsc_per_ms;
}
