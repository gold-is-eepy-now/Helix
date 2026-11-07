/* klog.h — the kernel event log.
 *
 * Every subsystem records what it does into a fixed ring of recent events. The ring
 * is kept in memory and printed when something goes wrong: on a CPU exception, or
 * on demand with the `log` command. Recording never writes to the serial port, so
 * it does not change the timing of a busy system the way printing would.
 *
 * Two entry points:
 *   klog_event  records into the ring only (for frequent events such as switches).
 *   klog_note   records, and also prints on serial when the category is enabled
 *               (for milestones such as a process starting or the desktop opening).
 */
#ifndef HELIX_KLOG_H
#define HELIX_KLOG_H

#include <stdarg.h>
#include <stdint.h>

typedef enum {
    KLOG_SCHED   = 1u << 0,   /* thread creation, preemption, block and resume */
    KLOG_SYSCALL = 1u << 1,   /* every system call: number, arguments and result */
    KLOG_PROC    = 1u << 2,   /* process spawn and exit */
    KLOG_SHELL   = 1u << 3,   /* kernel shell: each line and command */
    KLOG_DESKTOP = 1u << 4,   /* the graphical desktop */
    KLOG_INPUT   = 1u << 5,   /* keys and mouse events */
    KLOG_USB     = 1u << 6,   /* USB mouse motion */
    KLOG_EXC     = 1u << 7,   /* CPU exceptions */
} klog_cat_t;

#define KLOG_ALL 0xFFu

/* Record an event in the ring only. */
void klog_event(klog_cat_t cat, const char *fmt, ...);

/* Record an event, and print it on serial when its category is in the serial mask.
 * The serial text is the formatted message as given (no prefix), so existing
 * markers such as "[desktop] started" stay greppable. */
void klog_note(klog_cat_t cat, const char *fmt, ...);

/* Print the ring to the console, oldest event first. */
void klog_dump(void);

/* Forget every recorded event (the sequence numbers keep counting). */
void klog_clear(void);

/* Which categories klog_note prints on serial. Default: process, shell, desktop, exceptions. */
void klog_set_serial(uint32_t mask);
uint32_t klog_serial_mask(void);

#endif /* HELIX_KLOG_H */
