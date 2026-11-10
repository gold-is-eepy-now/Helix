/* t2-unit.c — host-side tests for the Apple T2 BCE mailbox protocol.
 *
 * t2_protocol.c is pure logic (message encoding plus register reads against a
 * caller supplied block), so it can be compiled for the host and driven with
 * a fake register file. The QEMU boot proves the kernel driver links and
 * runs; this file proves the wire format is right.
 * Built and run by `make test`.
 */
#include <stdio.h>
#include <stdint.h>

#include "t2_protocol.h"

static int fails;
#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fails++;                                                       \
            printf("FAIL: " __VA_ARGS__);                                  \
            printf("\n");                                                  \
        }                                                                  \
    } while (0)

/* Fake BAR4: large enough to cover the timestamp page at 0xC000. */
#define REG_WORDS 0x400u
static uint32_t regs[REG_WORDS];

#define REG(off) (regs[(off) / 4u])

static void reset_regs(void) {
    for (unsigned i = 0; i < REG_WORDS; i++) regs[i] = 0;
}

int main(void) {
    t2_mailbox_t mb;

    /* Message encoding: 6-bit type above a 58-bit value. */
    uint64_t msg = T2_MB_MSG(T2_MB_SET_FW_PROTOCOL_VERSION, T2_FW_PROTOCOL_VERSION);
    CHECK(T2_MB_TYPE(msg) == T2_MB_SET_FW_PROTOCOL_VERSION,
          "message type round-trips (got 0x%llx)",
          (unsigned long long)T2_MB_TYPE(msg));
    CHECK(T2_MB_VALUE(msg) == T2_FW_PROTOCOL_VERSION,
          "message value round-trips (got 0x%llx)",
          (unsigned long long)T2_MB_VALUE(msg));
    CHECK(T2_MB_TYPE(T2_MB_MSG(0x3F, UINT64_C(0x03FFFFFFFFFFFFFF))) == 0x3F,
          "largest type and value pack without overlap");

    /* Sending writes lo, hi and two padding words to the out box. */
    reset_regs();
    t2_mailbox_init(&mb, regs);
    t2_mailbox_send(&mb, msg);
    CHECK(REG(T2_MBOX_OUT_BASE + 0) == (uint32_t)(msg & 0xFFFFFFFFu),
          "out box low word");
    CHECK(REG(T2_MBOX_OUT_BASE + 4) == (uint32_t)(msg >> 32),
          "out box high word");
    CHECK(REG(T2_MBOX_OUT_BASE + 8) == 0 && REG(T2_MBOX_OUT_BASE + 12) == 0,
          "out box padding words are zero");

    /* Reply counter: pending count lives in bits 23:20. */
    reset_regs();
    t2_mailbox_init(&mb, regs);
    CHECK(t2_mailbox_reply_count(&mb) == 0, "no replies pending initially");
    REG(T2_MBOX_REPLY_COUNTER) = 2u << T2_MBOX_REPLY_COUNT_SHIFT;
    CHECK(t2_mailbox_reply_count(&mb) == 2, "pending reply count is decoded");
    REG(T2_MBOX_REPLY_COUNTER) = 0xFFFFFFFFu;
    CHECK(t2_mailbox_reply_count(&mb) == T2_MBOX_REPLY_COUNT_MASK,
          "reply count is masked to four bits");

    /* Reading a reply composes the 64-bit message from the reply slot. */
    reset_regs();
    t2_mailbox_init(&mb, regs);
    uint64_t reply = 0;
    CHECK(!t2_mailbox_take_reply(&mb, &reply), "no reply when the counter is empty");
    REG(T2_MBOX_REPLY_BASE + 0) = (uint32_t)(msg & 0xFFFFFFFFu);
    REG(T2_MBOX_REPLY_BASE + 4) = (uint32_t)(msg >> 32);
    REG(T2_MBOX_REPLY_COUNTER) = 1u << T2_MBOX_REPLY_COUNT_SHIFT;
    CHECK(t2_mailbox_take_reply(&mb, &reply), "reply is taken when one is pending");
    CHECK(reply == msg, "reply message matches what was sent (got 0x%llx)",
          (unsigned long long)reply);

    /* The firmware handshake only succeeds on the exact type and version. */
    CHECK(t2_fw_handshake_valid(msg), "handshake accepts protocol 0x20001");
    CHECK(!t2_fw_handshake_valid(T2_MB_MSG(T2_MB_SET_FW_PROTOCOL_VERSION, 0x20002u)),
          "handshake rejects another protocol version");
    CHECK(!t2_fw_handshake_valid(T2_MB_MSG(0xDu, T2_FW_PROTOCOL_VERSION)),
          "handshake rejects another message type");

    printf("%s: %d failure(s)\n",
           fails ? "FAILED" : "all T2 mailbox checks passed", fails);
    return fails != 0;
}
