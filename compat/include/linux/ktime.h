/* linux/ktime.h — nanosecond timestamps from the TSC-calibrated uptime clock. */
#ifndef HELIX_COMPAT_LINUX_KTIME_H
#define HELIX_COMPAT_LINUX_KTIME_H
#include <linux/types.h>
typedef s64 ktime_t;
ktime_t ktime_get(void);
static inline ktime_t ktime_get_coarse(void) { return ktime_get(); }
static inline s64 ktime_ms_delta(ktime_t later, ktime_t earlier) { return (later - earlier) / 1000000; }
#endif
