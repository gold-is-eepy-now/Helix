/* linux/atomic.h — atomic integer operations, built on GCC's __atomic builtins. */
#ifndef HELIX_COMPAT_LINUX_ATOMIC_H
#define HELIX_COMPAT_LINUX_ATOMIC_H

#include <linux/types.h>

typedef struct {
    volatile int counter;
} atomic_t;

#define ATOMIC_INIT(i) { (i) }

static inline int atomic_read(const atomic_t *v) {
    return __atomic_load_n(&v->counter, __ATOMIC_SEQ_CST);
}

static inline void atomic_set(atomic_t *v, int i) {
    __atomic_store_n(&v->counter, i, __ATOMIC_SEQ_CST);
}

static inline void atomic_add(int i, atomic_t *v) {
    __atomic_fetch_add(&v->counter, i, __ATOMIC_SEQ_CST);
}

static inline void atomic_sub(int i, atomic_t *v) {
    __atomic_fetch_sub(&v->counter, i, __ATOMIC_SEQ_CST);
}

static inline void atomic_inc(atomic_t *v) {
    atomic_add(1, v);
}

static inline void atomic_dec(atomic_t *v) {
    atomic_sub(1, v);
}

static inline int atomic_add_return(int i, atomic_t *v) {
    return __atomic_add_fetch(&v->counter, i, __ATOMIC_SEQ_CST);
}

static inline int atomic_sub_return(int i, atomic_t *v) {
    return __atomic_sub_fetch(&v->counter, i, __ATOMIC_SEQ_CST);
}

static inline int atomic_inc_return(atomic_t *v) {
    return atomic_add_return(1, v);
}

static inline int atomic_dec_return(atomic_t *v) {
    return atomic_sub_return(1, v);
}

#endif /* HELIX_COMPAT_LINUX_ATOMIC_H */
