/* linux/kernel.h — printk and the logging and assertion helpers drivers call. */
#ifndef HELIX_COMPAT_LINUX_KERNEL_H
#define HELIX_COMPAT_LINUX_KERNEL_H

#include <linux/errno.h>
#include <linux/types.h>
#include "panic.h"   /* Helix's kprintf and panic */

/* Log levels: Linux prefixes messages with a one-byte level. Helix has no
 * levels yet, so the prefixes are empty and pr_* adds a driver tag. */
#define KERN_EMERG   ""
#define KERN_ALERT   ""
#define KERN_CRIT    ""
#define KERN_ERR     ""
#define KERN_WARNING ""
#define KERN_NOTICE  ""
#define KERN_INFO    ""
#define KERN_DEBUG   ""

#define printk(...) kprintf(__VA_ARGS__)

#ifndef pr_fmt
#define pr_fmt(fmt) fmt
#endif

#define pr_info(fmt, ...)  kprintf("[compat] " pr_fmt(fmt), ##__VA_ARGS__)
#define pr_warn(fmt, ...)  kprintf("[compat] warning: " pr_fmt(fmt), ##__VA_ARGS__)
#define pr_err(fmt, ...)   kprintf("[compat] error: " pr_fmt(fmt), ##__VA_ARGS__)
#define pr_debug(fmt, ...) do { } while (0)

#define BUG_ON(cond) do { if (unlikely(cond)) panic("BUG_ON: " #cond); } while (0)
#define WARN_ON(cond) ({ int __w = !!(cond); if (unlikely(__w)) \
    kprintf("[compat] WARN_ON: %s\n", #cond); __w; })

#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))

#endif /* HELIX_COMPAT_LINUX_KERNEL_H */
