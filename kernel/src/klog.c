/* klog.c — the kernel event ring (see klog.h).
 *
 * The ring holds the last KLOG_SLOTS events. Each event keeps its sequence number,
 * the uptime in milliseconds, its category and a short formatted text. Formatting
 * uses kvprintf_to, so the text is produced by the same code as kprintf.
 */
#include "klog.h"

#include <stddef.h>

#include "panic.h"
#include "time.h"

#define KLOG_SLOTS 512
#define KLOG_TEXT  112

typedef struct {
    uint64_t seq;
    uint64_t ms;
    uint32_t cat;
    char text[KLOG_TEXT];
} klog_slot_t;

static klog_slot_t g_ring[KLOG_SLOTS];
static uint64_t g_seq;   /* events recorded so far; the newest is g_seq - 1 */
static uint64_t g_first; /* the oldest event a dump may show (set by klog_clear) */
static uint32_t g_serial_mask = KLOG_PROC | KLOG_SHELL | KLOG_DESKTOP | KLOG_EXC;

/* Writes into a bounded buffer; characters past the end are dropped. */
typedef struct {
    char *buf;
    size_t cap;
    size_t len;
} klog_text_t;

static void text_putc(char c, void *ctx) {
    klog_text_t *t = ctx;
    if (t->len + 1 < t->cap) {
        t->buf[t->len++] = c;
    }
}

/* Record one event. The slot is claimed before formatting, so an interrupt that
 * records during formatting takes the next slot rather than this one. */
static klog_slot_t *record(klog_cat_t cat, const char *fmt, va_list args) {
    klog_slot_t *slot = &g_ring[g_seq % KLOG_SLOTS];
    slot->seq = g_seq;
    slot->ms = time_uptime_ms();
    slot->cat = (uint32_t)cat;
    g_seq++;
    klog_text_t t = { slot->text, KLOG_TEXT, 0 };
    kvprintf_to(text_putc, &t, fmt, args);
    /* Messages written for the serial console end in a newline; the ring keeps
     * one event per line, so the newline is dropped here. */
    while (t.len > 0 && slot->text[t.len - 1] == '\n') {
        t.len--;
    }
    slot->text[t.len] = '\0';
    return slot;
}

void klog_event(klog_cat_t cat, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    record(cat, fmt, args);
    va_end(args);
}

void klog_note(klog_cat_t cat, const char *fmt, ...) {
    va_list args, serial;
    va_start(args, fmt);
    va_copy(serial, args);
    record(cat, fmt, args);
    if (g_serial_mask & (uint32_t)cat) {
        kvprintf_to(NULL, NULL, fmt, serial);   /* NULL sink: the console */
    }
    va_end(args);
    va_end(serial);
}

static const char *cat_name(uint32_t cat) {
    switch (cat) {
        case KLOG_SCHED:   return "sched";
        case KLOG_SYSCALL: return "syscall";
        case KLOG_PROC:    return "proc";
        case KLOG_SHELL:   return "shell";
        case KLOG_DESKTOP: return "desktop";
        case KLOG_INPUT:   return "input";
        case KLOG_USB:     return "usb";
        case KLOG_EXC:     return "exc";
        default:           return "?";
    }
}

void klog_dump(void) {
    uint64_t count = g_seq - g_first < KLOG_SLOTS ? g_seq - g_first : KLOG_SLOTS;
    uint64_t first = g_seq - count;
    kprintf("\n[klog] last %llu events, oldest first:\n", (unsigned long long)count);
    for (uint64_t i = first; i < g_seq; i++) {
        const klog_slot_t *slot = &g_ring[i % KLOG_SLOTS];
        kprintf("[klog #%llu %llu ms %s] %s\n", (unsigned long long)slot->seq,
                (unsigned long long)slot->ms, cat_name(slot->cat), slot->text);
    }
    kprintf("[klog] end\n");
}

void klog_clear(void) {
    /* Start the window from here: the next dump shows events after this point. */
    g_first = g_seq;
}

void klog_set_serial(uint32_t mask) {
    g_serial_mask = mask;
}

uint32_t klog_serial_mask(void) {
    return g_serial_mask;
}
