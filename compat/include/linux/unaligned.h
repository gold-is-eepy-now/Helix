/* linux/unaligned.h — loads and stores that may be misaligned (x86 tolerates them, but the
 * memcpy form is also correct C). */
#ifndef HELIX_COMPAT_LINUX_UNALIGNED_H
#define HELIX_COMPAT_LINUX_UNALIGNED_H
#include <linux/types.h>
static inline u16 get_unaligned_le16(const void *p) { const u8 *b = p; return (u16)(b[0] | (b[1] << 8)); }
static inline void put_unaligned_le16(u16 v, void *p) { u8 *b = p; b[0] = (u8)v; b[1] = (u8)(v >> 8); }
static inline u32 get_unaligned_le32(const void *p) {
    const u8 *b = p; return (u32)b[0] | ((u32)b[1] << 8) | ((u32)b[2] << 16) | ((u32)b[3] << 24);
}
#endif
