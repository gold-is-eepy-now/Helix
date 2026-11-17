/* linux/crc16.h — CRC-16 (polynomial 0xA001 reflected, i.e. CRC-16/ARC), as Linux's lib/crc16. */
#ifndef HELIX_COMPAT_LINUX_CRC16_H
#define HELIX_COMPAT_LINUX_CRC16_H
#include <linux/types.h>
u16 crc16(u16 crc, const u8 *buffer, size_t len);
#endif
