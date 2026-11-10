/* net_icmp.c — ICMP echo request and reply (see net_icmp.h).
 *
 * ICMP echo header: 0 type (8 = request, 0 = reply)   1 code (0)
 *                   2 checksum (over the whole message)   4 identifier   6 sequence
 * followed by the payload, which a reply returns unchanged. */
#include "net_icmp.h"

#include <stddef.h>

#include "net.h"
#include "net_ip.h"
#include "time.h"

#define ICMP_HLEN        8
#define ICMP_ECHO_REPLY  0
#define ICMP_ECHO_REQ    8
#define PING_ID          0x4845u    /* "HE": identifies our requests among other pings */
#define PING_PAYLOAD     32
#define ECHO_REPLY_MAX   256        /* largest echo we will answer, bytes */

typedef struct {
    bool waiting;                   /* a net_ping call is waiting for a reply */
    bool got;                       /* its reply has arrived */
    uint16_t seq;
    uint8_t dst[4];
} ping_state_t;

static ping_state_t g_ping;

static bool same4(const uint8_t *a, const uint8_t *b) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

void net_icmp_input(const uint8_t src[4], const uint8_t *msg, uint16_t len) {
    if (len < ICMP_HLEN) return;
    if (net_checksum(msg, len) != 0) return;            /* damaged: drop silently */

    if (msg[0] == ICMP_ECHO_REQ && msg[1] == 0) {
        if (len > ECHO_REPLY_MAX) return;
        /* A local copy: this runs inside net_poll, and sending may poll again. */
        uint8_t reply[ECHO_REPLY_MAX];
        for (uint16_t i = 0; i < len; i++) reply[i] = msg[i];
        reply[0] = ICMP_ECHO_REPLY;
        reply[2] = 0;
        reply[3] = 0;
        net_put16(reply + 2, net_checksum(reply, len));
        net_ip_send(src, NET_IP_PROTO_ICMP, reply, len);
        return;
    }

    if (msg[0] == ICMP_ECHO_REPLY && msg[1] == 0 && g_ping.waiting && !g_ping.got) {
        if (net_get16(msg + 4) == PING_ID && net_get16(msg + 6) == g_ping.seq &&
            same4(src, g_ping.dst)) {
            g_ping.got = true;
        }
    }
}

bool net_ping(const uint8_t dst[4], uint16_t seq, uint32_t timeout_ms, uint32_t *rtt_ms) {
    static uint8_t msg[ICMP_HLEN + PING_PAYLOAD];
    if (!net_ready() || !time_available()) return false;

    msg[0] = ICMP_ECHO_REQ;
    msg[1] = 0;
    msg[2] = 0;
    msg[3] = 0;
    net_put16(msg + 4, PING_ID);
    net_put16(msg + 6, seq);
    for (int i = 0; i < PING_PAYLOAD; i++) msg[ICMP_HLEN + i] = (uint8_t)i;
    net_put16(msg + 2, net_checksum(msg, sizeof msg));

    g_ping.waiting = true;
    g_ping.got = false;
    g_ping.seq = seq;
    for (int i = 0; i < 4; i++) g_ping.dst[i] = dst[i];

    uint64_t start = time_uptime_ms();
    if (!net_ip_send(dst, NET_IP_PROTO_ICMP, msg, sizeof msg)) {
        g_ping.waiting = false;
        return false;
    }
    for (;;) {
        net_poll();
        if (g_ping.got) break;
        if (time_uptime_ms() - start >= timeout_ms) {
            g_ping.waiting = false;
            return false;
        }
    }
    g_ping.waiting = false;
    *rtt_ms = (uint32_t)(time_uptime_ms() - start);
    return true;
}
