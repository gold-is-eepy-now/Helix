/* linux/workqueue.h — deferred work. schedule_work() queues the item; compat_work_run()
 * (called from the SPI pump and the self-test) runs what is queued. */
#ifndef HELIX_COMPAT_LINUX_WORKQUEUE_H
#define HELIX_COMPAT_LINUX_WORKQUEUE_H
#include <linux/types.h>
struct work_struct {
    void (*func)(struct work_struct *work);
    volatile int pending;
    struct work_struct *next;
};
#define INIT_WORK(w, fn) do { (w)->func = (fn); (w)->pending = 0; (w)->next = NULL; } while (0)
bool schedule_work(struct work_struct *work);
int compat_work_run(void);
#endif
