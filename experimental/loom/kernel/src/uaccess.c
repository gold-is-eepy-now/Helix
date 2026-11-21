/* Copying between the kernel and a Cell's memory. The kernel never dereferences a user pointer
 * directly: each byte is translated through the Cell's own page tables first, so an address the
 * Cell does not map (or a kernel address) is refused rather than read. Byte-at-a-time is slow and
 * correct; the sizes here are at most a few dozen bytes. */
#include <stdint.h>
#include "loom_cell.h"
#include "loom_kernel.h"

#define USER_LIMIT 0x0000800000000000ull

static bool phys_of(const struct loom_cell *c, uint64_t va, uint8_t **out) {
    if (va >= USER_LIMIT) return false;
    uint64_t pa = vm_translate(c->ctx.user_cr3, va);
    if (pa == 0) return false;
    *out = (uint8_t *)phys_to_virt(pa);
    return true;
}

bool copy_from_user(const struct loom_cell *c, void *dst, uint64_t src, size_t n) {
    uint8_t *d = dst;
    for (size_t i = 0; i < n; i++) {
        uint8_t *p;
        if (!phys_of(c, src + i, &p)) return false;
        d[i] = *p;
    }
    return true;
}

bool copy_to_user(const struct loom_cell *c, uint64_t dst, const void *src, size_t n) {
    const uint8_t *s = src;
    for (size_t i = 0; i < n; i++) {
        uint8_t *p;
        if (!phys_of(c, dst + i, &p)) return false;
        *p = s[i];
    }
    return true;
}
