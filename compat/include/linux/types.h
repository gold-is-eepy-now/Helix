/* linux/types.h — the Linux type names drivers use, mapped onto Helix's types.
 *
 * Part of the Linux compatibility layer (docs/08-linux-compat.md). Drivers
 * include <linux/...> headers; they never see Helix's own kernel headers. */
#ifndef HELIX_COMPAT_LINUX_TYPES_H
#define HELIX_COMPAT_LINUX_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;
typedef uint32_t __le32;
typedef uint16_t __le16;
typedef uint64_t __le64;
typedef uint32_t __be32;
typedef uint16_t __be16;
typedef uint32_t gfp_t;
typedef int64_t  loff_t;

#define __init
#define __exit
#define __initdata
#define __user
#define __iomem
#define __maybe_unused __attribute__((unused))
#define __always_inline inline __attribute__((always_inline))

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

#endif /* HELIX_COMPAT_LINUX_TYPES_H */
