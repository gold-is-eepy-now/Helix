/* linux/mutex.h — sleeping mutex. Helix's scheduler can yield, so a contended
 * mutex yields the CPU until the holder releases it, instead of spinning. */
#ifndef HELIX_COMPAT_LINUX_MUTEX_H
#define HELIX_COMPAT_LINUX_MUTEX_H

#include <linux/types.h>
#include "thread.h"

typedef struct {
    volatile int locked;
} mutex_t;

#define DEFINE_MUTEX(name) mutex_t name = { 0 }

static inline void mutex_init(mutex_t *m) {
    m->locked = 0;
}

static inline void mutex_lock(mutex_t *m) {
    while (__atomic_exchange_n(&m->locked, 1, __ATOMIC_ACQUIRE)) {
        thread_yield();
    }
}

static inline int mutex_trylock(mutex_t *m) {
    return __atomic_exchange_n(&m->locked, 1, __ATOMIC_ACQUIRE) == 0;
}

static inline void mutex_unlock(mutex_t *m) {
    __atomic_store_n(&m->locked, 0, __ATOMIC_RELEASE);
}

#endif /* HELIX_COMPAT_LINUX_MUTEX_H */
