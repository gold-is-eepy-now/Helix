#ifndef HELIX_COMPAT_ASM_BARRIER_H
#define HELIX_COMPAT_ASM_BARRIER_H
#define smp_load_acquire(p) __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define smp_store_release(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#endif
