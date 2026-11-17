/* linux/compat_common.h — small helpers Linux drivers expect from <linux/bitops.h>,
 * <linux/err.h>, <linux/byteorder> and the compiler headers, in one place.
 * Part of the Linux compatibility layer (docs/08-linux-compat.md). Written for Helix;
 * only the names and meanings follow Linux. */
#ifndef HELIX_COMPAT_COMMON_H
#define HELIX_COMPAT_COMMON_H

#include <linux/types.h>
#include <stdint.h>

#define BIT(n) (1UL << (n))
#define __packed __attribute__((packed))
#define __aligned(x) __attribute__((aligned(x)))
#define __always_unused __attribute__((unused))
#define sizeof_field(type, member) (sizeof(((type *)0)->member))

/* x86 is little-endian, so the le16/le32 conversions are the identity. */
#define le16_to_cpu(x) ((u16)(x))
#define le32_to_cpu(x) ((u32)(x))
#define le64_to_cpu(x) ((u64)(x))
#define cpu_to_le16(x) ((__le16)(x))
#define cpu_to_le32(x) ((__le32)(x))
#define cpu_to_le64(x) ((__le64)(x))

/* Bit arrays are arrays of unsigned long, as in Linux. */
#define COMPAT_BITS_PER_LONG (8 * sizeof(unsigned long))
static inline void __set_bit(unsigned int nr, unsigned long *addr) {
    addr[nr / COMPAT_BITS_PER_LONG] |= 1ul << (nr % COMPAT_BITS_PER_LONG);
}
static inline void __clear_bit(unsigned int nr, unsigned long *addr) {
    addr[nr / COMPAT_BITS_PER_LONG] &= ~(1ul << (nr % COMPAT_BITS_PER_LONG));
}
static inline int test_bit(unsigned int nr, const unsigned long *addr) {
    return (int)((addr[nr / COMPAT_BITS_PER_LONG] >> (nr % COMPAT_BITS_PER_LONG)) & 1ul);
}
#define set_bit __set_bit
#define clear_bit __clear_bit

/* ERR_PTR: errors encoded in the top 4095 values of the address space. */
#define MAX_ERRNO 4095
static inline void *ERR_PTR(long error) { return (void *)error; }
static inline long PTR_ERR(const void *ptr) { return (long)ptr; }
static inline bool IS_ERR(const void *ptr) { return (unsigned long)ptr >= (unsigned long)-MAX_ERRNO; }
static inline bool IS_ERR_OR_NULL(const void *ptr) { return !ptr || IS_ERR(ptr); }

/* A trailing flexible array that may sit inside a union (Linux's DECLARE_FLEX_ARRAY). */
#define DECLARE_FLEX_ARRAY(type, name) \
    struct { struct { } __empty_##name; type name[]; }

#define struct_size(p, member, n) (sizeof(*(p)) + (size_t)(n) * sizeof((p)->member[0]))
#define compiletime_assert(cond, msg) _Static_assert(cond, msg)

/* memchr_inv: first byte of buf that differs from c, or NULL if all equal. */
void *memchr_inv(const void *buf, int c, size_t n);

/* Formatting: Helix's kprintf supports only %s %c %d %u %x %p and l/ll. snprintf here
 * uses that same formatter, so %*ph and wider specs print as the formatter prints them. */
int snprintf(char *buf, size_t size, const char *fmt, ...);
int sscanf(const char *buf, const char *fmt, ...);

#define WARN_ONCE(cond, ...) ({ \
    static bool __warned; int __c = !!(cond); \
    if (__c && !__warned) { __warned = true; kprintf("[compat] WARN: " __VA_ARGS__); kprintf("\n"); } \
    __c; })

/* Cleanup-based scope guards (Linux's <linux/cleanup.h>), via GCC's cleanup attribute. */
#define __compat_cleanup(fn) __attribute__((cleanup(fn)))
#define __COMPAT_CAT2(a, b) a##b
#define __COMPAT_CAT(a, b) __COMPAT_CAT2(a, b)
#define __COMPAT_UID(prefix) __COMPAT_CAT(prefix, __COUNTER__)
#define CLASS(name, var) \
    class_##name##_t var __compat_cleanup(class_##name##_destructor) = class_##name##_constructor
#define guard(name) CLASS(name, __COMPAT_UID(__guard_))
#define scoped_guard(name, ...) \
    for (int __sg_once = 1; __sg_once;) \
        for (CLASS(name, __sg)(__VA_ARGS__); __sg_once; __sg_once = 0)

#endif /* HELIX_COMPAT_COMMON_H */
