/* hello_compat.c — the first Linux-compatibility driver: a self-test written the
 * way a Linux driver is written, against <linux/...> headers only. It exercises
 * the first API set: slab allocation, a spinlock with irqsave, atomic counters,
 * and printk. It does not touch hardware. */
#include <linux/atomic.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Helix");
MODULE_DESCRIPTION("Linux compatibility layer self-test (slab, spinlock, atomic, printk)");

static DEFINE_SPINLOCK(hello_lock);
static atomic_t hello_hits = ATOMIC_INIT(0);

static int hello_selftest(void) {
    unsigned long flags;
    int *buf = kzalloc(8 * sizeof(*buf), GFP_KERNEL);
    if (buf == NULL) {
        return -ENOMEM;
    }

    spin_lock_irqsave(&hello_lock, flags);
    buf[0] = 42;
    atomic_inc(&hello_hits);
    spin_unlock_irqrestore(&hello_lock, flags);

    int ok = buf[0] == 42 && buf[7] == 0 && atomic_read(&hello_hits) == 1;
    kfree(buf);
    if (!ok) {
        pr_err("self-test failed\n");
        return -EIO;
    }
    pr_info("self-test passed (slab, spinlock, atomic, printk)\n");
    return 0;
}
module_init(hello_selftest);
