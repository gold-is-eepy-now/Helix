/* linux/slab.h — memory allocation. Helix has one heap, so the GFP flags are
 * accepted and ignored for now: every allocation comes from kmalloc's pool. */
#ifndef HELIX_COMPAT_LINUX_SLAB_H
#define HELIX_COMPAT_LINUX_SLAB_H

#include <linux/types.h>

#define GFP_KERNEL 0u
#define GFP_ATOMIC 0u
#define GFP_NOWAIT 0u
#define __GFP_ZERO 0u

void *compat_kmalloc(size_t size);
void *compat_kzalloc(size_t size);
void compat_kfree(void *ptr);

static inline void *kmalloc(size_t size, gfp_t flags) {
    (void)flags;
    return compat_kmalloc(size);
}

static inline void *kzalloc(size_t size, gfp_t flags) {
    (void)flags;
    return compat_kzalloc(size);
}

static inline void *kcalloc(size_t n, size_t size, gfp_t flags) {
    (void)flags;
    if (size != 0 && n > SIZE_MAX / size) return NULL;
    return compat_kzalloc(n * size);
}

static inline void kfree(const void *ptr) {
    compat_kfree((void *)ptr);
}

#endif /* HELIX_COMPAT_LINUX_SLAB_H */
