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

uint64_t time_tsc_per_ms(void) {
    return g_tsc_per_ms;
}

uint64_t time_uptime_ms(void) {
    if (g_tsc_per_ms == 0) {
        return 0;
    }
    return (rdtsc() - g_boot_tsc) / g_tsc_per_ms;
}

static uint64_t g_boot_epoch;     /* wall-clock seconds at time_set_boot_epoch() */
static uint64_t g_boot_epoch_up;  /* uptime (ms) when that epoch was recorded */

void time_set_boot_epoch(uint64_t epoch_seconds) {
    g_boot_epoch = epoch_seconds;
    g_boot_epoch_up = time_uptime_ms();
}

static uint64_t cmos_read(uint8_t idx) {
    outb(0x70, (uint8_t)(idx | 0x80)); /* bit 7 disables the NMI */
    for (volatile int i = 0; i < 16; i++) { /* tiny pause */ }
    return inb(0x71);
}

static uint8_t bcd2bin(uint8_t x) {
    return (uint8_t)(((x >> 4) & 0x0F) * 10u + (x & 0x0F));
}

/* Days between 1970-01-01 and y-m-d (proleptic Gregorian). */
static int64_t days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + doe - 719468;
}

/* Inverse of days_from_civil(). */
static void civil_from_days(int64_t z, int *y, int *m, int *d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    uint64_t doe = (uint64_t)(z - era * 146097);
    int yoe = (int)((doe - doe / 1460 + doe / 36524 - doe / 146096) / 365);
    int yr = yoe + (int)(era * 400);
    int doy = (int)(doe - (uint64_t)(yoe * 365 + yoe / 4 - yoe / 100));
    int mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    if (*m <= 2) yr += 1;
    *y = yr;
}

uint64_t time_rtc_epoch(void) {
    /* ponytail: the CMOS clock is read as UTC, matching Limine's own
     * "clock holds UTC" choice. Boards that keep local time need an offset
     * here once a timezone setting exists. */
    /* Wait out the update cycle (update-in-progress bit in register A). */
    for (int i = 0; i < 128; i++) {
        if (!(cmos_read(0x0A) & 0x80)) break;
    }

    uint8_t reg_b = cmos_read(0x0B);
    int binary = (reg_b & 0x04) ? 1 : 0;
    int hour24 = (reg_b & 0x02) ? 1 : 0;

    uint8_t second = cmos_read(0x00);
    uint8_t minute = cmos_read(0x02);
    uint8_t raw_hour = cmos_read(0x04);
    uint8_t day = cmos_read(0x07);
    uint8_t month = cmos_read(0x08);
    uint8_t year = cmos_read(0x09);
    uint8_t century = cmos_read(0x32); /* not present on every chipset */

    int pm = (raw_hour & 0x80) ? 1 : 0;
    uint8_t hour = raw_hour & 0x7F;

    if (!binary) {
        second = bcd2bin(second);
        minute = bcd2bin(minute);
        hour = bcd2bin(hour);
        day = bcd2bin(day);
        month = bcd2bin(month);
        year = bcd2bin(year);
        century = bcd2bin(century);
    }
    if (!hour24) {
        if (pm && hour != 12) hour = (uint8_t)((hour % 12) + 12);
        if (!pm && hour == 12) hour = 0;
    }

    /* Reject nonsense readings: no clock beats a wrong clock. */
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 ||
        minute > 59 || second > 60 || year > 99) {
        return 0;
    }

    int full_year;
    if (century >= 1 && century <= 99) {
        full_year = century * 100 + year;
    } else {
        full_year = (year < 70) ? 2000 + year : 1900 + year;
    }

    int64_t days = days_from_civil(full_year, month, day);
    if (days < 0) return 0;
    return (uint64_t)days * 86400u + (uint64_t)hour * 3600u +
           (uint64_t)minute * 60u + second;
}

uint64_t time_epoch(void) {
    if (g_boot_epoch) {
        /* The epoch was sampled a moment after boot; add the time since. */
        uint64_t elapsed = time_uptime_ms() - g_boot_epoch_up;
        return g_boot_epoch + elapsed / 1000u;
    }
    uint64_t e = time_rtc_epoch();
    if (e) return e;
    return 0;
}

void time_now_utc(int *year, int *month, int *day,
                  int *hour, int *minute, int *second,
                  int *weekday) {
    int y = 0, m = 0, d = 0, h = 0, mi = 0, s = 0, w = 0;
    uint64_t e = time_epoch();
    if (e) {
        int64_t days = (int64_t)(e / 86400u);
        uint64_t rem = e % 86400u;
        h = (int)(rem / 3600u);
        mi = (int)((rem % 3600u) / 60u);
        s = (int)(rem % 60u);
        civil_from_days(days, &y, &m, &d);
        /* Monday = 1 via epoch: (days+4) % 7 gives Sun=0 */
        w = (int)((days + 4) % 7);
    }
    if (year) *year = y;
    if (month) *month = m;
    if (day) *day = d;
    if (hour) *hour = h;
    if (minute) *minute = mi;
    if (second) *second = s;
    if (weekday) *weekday = w;
}
