/* misc_shim.c — time, delays, CRC, formatted output, leds, debugfs, workqueue and EFI stubs
 * the Linux driver calls. Each one says in its comment what Helix actually does. */
#include <stdarg.h>
#include <linux/types.h>
#include <linux/ktime.h>
#include <linux/delay.h>
#include <linux/crc16.h>
#include <linux/leds.h>
#include <linux/debugfs.h>
#include <linux/efi.h>
#include <linux/workqueue.h>
#include <linux/compat_common.h>
#include "time.h"
#include "panic.h"

/* ---- time ---- */

/* Nanoseconds since boot, from the TSC-calibrated uptime clock (millisecond resolution). */
ktime_t ktime_get(void) {
    return (ktime_t)time_uptime_ms() * 1000000;
}

/* Busy-wait. Compat drivers cannot sleep yet, so msleep() spins; a long msleep() blocks the
 * whole kernel for that time. The spin uses the TSC when it has been calibrated. */
void compat_delay_us(unsigned long us) {
    uint64_t per_ms = time_tsc_per_ms();
    if (per_ms == 0) {
        for (volatile unsigned long i = 0; i < us * 100ul; i++) { }
        return;
    }
    uint64_t ticks = (uint64_t)us * per_ms / 1000u;
    uint64_t start = time_tsc();
    while (time_tsc() - start < ticks) {
        __asm__ volatile("pause");
    }
}

void compat_delay_ms(unsigned long ms) {
    compat_delay_us(ms * 1000ul);
}

/* ---- CRC-16 (reflected 0xA001, as lib/crc16 in Linux) ---- */

u16 crc16(u16 crc, const u8 *buffer, size_t len) {
    while (len--) {
        crc ^= *buffer++;
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 1) ? (u16)((crc >> 1) ^ 0xA001u) : (u16)(crc >> 1);
        }
    }
    return crc;
}

void *memchr_inv(const void *buf, int c, size_t n) {
    const unsigned char *p = buf;
    while (n--) {
        if (*p != (unsigned char)c) return (void *)p;
        p++;
    }
    return NULL;
}

/* ---- formatting: built on the kernel's kprintf formatter, so the same specs apply ---- */

struct snbuf { char *buf; size_t size; size_t n; };

static void snbuf_put(char c, void *ctx) {
    struct snbuf *b = ctx;
    if (b->size && b->n + 1 < b->size) b->buf[b->n] = c;
    b->n++;
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    struct snbuf b = { buf, size, 0 };
    va_list args;
    va_start(args, fmt);
    kvprintf_to(snbuf_put, &b, fmt, args);
    va_end(args);
    if (size) buf[b.n < size ? b.n : size - 1] = '\0';
    return (int)b.n;
}

/* Only %d, %u and literal characters are parsed; enough for "XxY+W+H" touchpad strings. */
int sscanf(const char *buf, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int matched = 0;
    const char *s = buf;
    for (const char *f = fmt; *f; f++) {
        if (*f == '%' && (f[1] == 'd' || f[1] == 'u')) {
            f++;
            while (*s == ' ') s++;
            long value = 0;
            int neg = 0, digits = 0;
            if (f[0] == 'd' && *s == '-') { neg = 1; s++; }
            while (*s >= '0' && *s <= '9') { value = value * 10 + (*s - '0'); s++; digits++; }
            if (digits == 0) break;
            int *out_i = NULL;
            unsigned *out_u = NULL;
            if (f[0] == 'd') out_i = va_arg(args, int *); else out_u = va_arg(args, unsigned *);
            if (out_i) *out_i = (int)(neg ? -value : value);
            if (out_u) *out_u = (unsigned)value;
            matched++;
        } else if (*f == ' ') {
            while (*s == ' ') s++;
        } else {
            if (*s != *f) break;
            s++;
        }
    }
    va_end(args);
    return matched;
}

/* ---- leds: recorded, not visible (no sysfs) ---- */

int devm_led_classdev_register(struct device *parent, struct led_classdev *led_cdev) {
    (void)parent;
    if (led_cdev == NULL) return -1;
    led_cdev->brightness = LED_OFF;
    return 0;
}

/* ---- debugfs: accepted and discarded (Helix has no debugfs) ---- */

ssize_t simple_read_from_buffer(void __user *to, size_t count, loff_t *ppos, const void *from, size_t available) {
    (void)to; (void)count; (void)ppos; (void)from; (void)available;
    return 0;
}

struct dentry *debugfs_create_dir(const char *name, struct dentry *parent) {
    static struct dentry root;
    (void)name; (void)parent;
    return &root;
}

void debugfs_create_bool(const char *name, unsigned short mode, struct dentry *parent, bool *value) {
    (void)name; (void)mode; (void)parent; (void)value;
}

struct dentry *debugfs_create_file(const char *name, unsigned short mode, struct dentry *parent,
                                   void *data, const struct file_operations *fops) {
    (void)name; (void)mode; (void)parent; (void)data; (void)fops;
    static struct dentry file;
    return &file;
}

void debugfs_remove_recursive(struct dentry *dentry) {
    (void)dentry;
}

/* ---- EFI runtime: Helix has no EFI runtime services table yet ---- */

struct efi_runtime efi;

bool efi_rt_services_supported(unsigned int mask) {
    (void)mask;
    return false;
}

/* ---- workqueue: a FIFO run from compat_spi_pump() ---- */

static struct work_struct *g_work_head, *g_work_tail;

bool schedule_work(struct work_struct *work) {
    if (work == NULL || work->pending) return false;
    work->pending = 1;
    work->next = NULL;
    if (g_work_tail) g_work_tail->next = work; else g_work_head = work;
    g_work_tail = work;
    return true;
}

int compat_work_run(void) {
    int ran = 0;
    while (g_work_head) {
        struct work_struct *w = g_work_head;
        g_work_head = w->next;
        if (g_work_head == NULL) g_work_tail = NULL;
        w->pending = 0;
        w->func(w);
        ran++;
    }
    return ran;
}

int nonseekable_open(struct inode *inode, struct file *filp) {
    (void)inode;
    (void)filp;
    return 0;
}

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = s;
    while (n--) {
        if (*p == (unsigned char)c) return (void *)p;
        p++;
    }
    return NULL;
}
