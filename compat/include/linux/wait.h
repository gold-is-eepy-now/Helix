/* linux/wait.h — wait queues. Helix has no sleep/wake primitive yet, so a waiter yields
 * the CPU and re-checks its condition; wake_up_all is therefore a no-op. */
#ifndef HELIX_COMPAT_LINUX_WAIT_H
#define HELIX_COMPAT_LINUX_WAIT_H
#include <linux/spinlock.h>
#include "thread.h"
typedef struct { int unused; } wait_queue_head_t;
static inline void init_waitqueue_head(wait_queue_head_t *wq) { wq->unused = 0; }
#define wake_up_all(wq) do { (void)(wq); } while (0)
/* Drop the (irq-saving) lock while waiting, retake it before re-checking, as Linux does. */
#define wait_event_lock_irq(wq, cond, lock) \
    do { (void)(wq); while (!(cond)) { compat_spin_release(&(lock)); thread_yield(); compat_spin_acquire(&(lock)); } } while (0)
#endif
