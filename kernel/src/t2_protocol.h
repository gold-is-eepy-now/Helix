/* t2_protocol.h — Apple T2 / iBridge Box Controller Engine (BCE) mailbox.
 *
 * Clean-room implementation. The register layout and message encoding below
 * come from freely available descriptions of the T2 BCE mailbox (notably the
 * t2linux "apple-bce-drv" project, GPLv2, consulted as a specification only —
 * no code copied).
 *
 * Kept free of kernel dependencies so the protocol can be unit tested on the
 * build host.
 */
#ifndef HELIX_T2_PROTOCOL_H
#define HELIX_T2_PROTOCOL_H

#include <stdbool.h>
#include <stdint.h>

/* PCI identity of the BCE mailbox function exposed by Apple T2 silicon. */
#define T2_PCI_VENDOR_APPLE 0x106Bu
#define T2_PCI_DEVICE_BCE   0x1801u

/* Mailbox register offsets inside BAR4. */
#define T2_MBOX_OUT_BASE       0x820u  /* 4 x u32: message lo/hi + padding */
#define T2_MBOX_REPLY_BASE     0x810u  /* reply slot: lo, hi, 2 unused u32 */
#define T2_MBOX_REPLY_COUNTER  0x108u  /* pending reply count in bits 23:20 */
#define T2_MBOX_TIMESTAMP_BASE 0xC000u /* keepalive timer, second page */

/* Mailbox message types. */
#define T2_MB_SET_FW_PROTOCOL_VERSION 0xCu

/* Firmware protocol version spoken by the T2 BCE firmware. */
#define T2_FW_PROTOCOL_VERSION 0x20001u

/* The mailbox is polled; give the firmware 200 ms to answer. */
#define T2_MBOX_TIMEOUT_MS 200u

/* A mailbox message packs a 6-bit type above a 58-bit value. */
#define T2_MB_MSG(type, value)                                             \
    (((uint64_t)(type) << 58) | ((uint64_t)(value) & UINT64_C(0x03FFFFFFFFFFFFFF)))
#define T2_MB_TYPE(message)  ((uint32_t)((message) >> 58))
#define T2_MB_VALUE(message) ((uint64_t)((message) & UINT64_C(0x03FFFFFFFFFFFFFF)))

/* Reply counter field: pending reply count, 4 bits wide. */
#define T2_MBOX_REPLY_COUNT_MASK  0xFu
#define T2_MBOX_REPLY_COUNT_SHIFT 20u

typedef struct {
    volatile uint32_t *regs; /* BAR4 base */
} t2_mailbox_t;

void t2_mailbox_init(t2_mailbox_t *mb, volatile uint32_t *regs);
void t2_mailbox_send(t2_mailbox_t *mb, uint64_t message);
uint32_t t2_mailbox_reply_count(const t2_mailbox_t *mb);
bool t2_mailbox_take_reply(const t2_mailbox_t *mb, uint64_t *message_out);
bool t2_fw_handshake_valid(uint64_t reply);

#endif /* HELIX_T2_PROTOCOL_H */
