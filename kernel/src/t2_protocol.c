/* t2_protocol.c — Apple T2 BCE mailbox wire protocol.
 *
 * Host-testable translation unit: no kernel headers, no MMIO outside the
 * caller supplied register block.
 */

#include "t2_protocol.h"

#include <stddef.h>

void t2_mailbox_init(t2_mailbox_t *mb, volatile uint32_t *regs) {
    if (mb == NULL) return;
    mb->regs = regs;
}

void t2_mailbox_send(t2_mailbox_t *mb, uint64_t message) {
    if (mb == NULL || mb->regs == NULL) return;
    volatile uint32_t *out = mb->regs + (T2_MBOX_OUT_BASE / 4u);
    out[0] = (uint32_t)message;
    out[1] = (uint32_t)(message >> 32);
    out[2] = 0;
    out[3] = 0;
    /* Publish the message before the caller starts polling for a reply. */
    __asm__ volatile("" ::: "memory");
}

uint32_t t2_mailbox_reply_count(const t2_mailbox_t *mb) {
    if (mb == NULL || mb->regs == NULL) return 0;
    uint32_t counter = mb->regs[T2_MBOX_REPLY_COUNTER / 4u];
    return (counter >> T2_MBOX_REPLY_COUNT_SHIFT) & T2_MBOX_REPLY_COUNT_MASK;
}

bool t2_mailbox_take_reply(const t2_mailbox_t *mb, uint64_t *message_out) {
    if (mb == NULL || mb->regs == NULL) return false;
    uint32_t count = t2_mailbox_reply_count(mb);
    if (count == 0) return false;

    /* The mailbox exposes a single reply slot. A pending count above zero
     * means the slot holds a message the host has not consumed yet. */
    volatile uint32_t *reply = mb->regs + (T2_MBOX_REPLY_BASE / 4u);
    uint32_t lo = reply[0];
    uint32_t hi = reply[1];
    (void)reply[2];
    (void)reply[3];
    if (message_out != NULL) {
        *message_out = ((uint64_t)hi << 32) | (uint64_t)lo;
    }
    return true;
}

bool t2_fw_handshake_valid(uint64_t reply) {
    return T2_MB_TYPE(reply) == T2_MB_SET_FW_PROTOCOL_VERSION &&
           T2_MB_VALUE(reply) == T2_FW_PROTOCOL_VERSION;
}
