/* net_dhcp.c — DHCP client (see net_dhcp.h).
 *
 * A DHCP message is a BOOTP message: a 236-byte fixed header, a 4-byte magic cookie,
 * then options (code, length, value...; 255 ends the list, 0 is padding).
 *   0 op (1 request, 2 reply)   1 hw type   2 hw length   4 transaction id (xid)
 *   10 flags (0x8000 = reply by broadcast)   16 yiaddr (the address offered)
 *   28 client MAC (16 bytes)   236 magic cookie 63 82 53 63   240 options
 */
#include "net_dhcp.h"

#include <stddef.h>

#include "klog.h"
#include "net.h"
#include "net_udp.h"
#include "time.h"

#define DHCP_SERVER_PORT   67
#define DHCP_CLIENT_PORT   68
#define DHCP_HDR           236
#define DHCP_OPTIONS       240
#define DHCP_MSG_MAX       576
#define DHCP_MIN_LEN       300     /* some servers and relays expect at least this */

#define OPT_PAD            0
#define OPT_SUBNET         1
#define OPT_ROUTER         3
#define OPT_DNS            6
#define OPT_REQUESTED_IP   50
#define OPT_LEASE_TIME     51
#define OPT_MSG_TYPE       53
#define OPT_SERVER_ID      54
#define OPT_PARAM_LIST     55
#define OPT_END            255

#define MSG_DISCOVER 1
#define MSG_OFFER    2
#define MSG_REQUEST  3
#define MSG_ACK      5

static const uint8_t k_magic[4] = {0x63, 0x82, 0x53, 0x63};
static const uint8_t k_zero[4] = {0, 0, 0, 0};
static const uint8_t k_broadcast[4] = {255, 255, 255, 255};

/* Append one option at off. Returns the new offset. */
static int put_option(uint8_t *m, int off, uint8_t code, uint8_t len, const uint8_t *val) {
    m[off++] = code;
    m[off++] = len;
    for (uint8_t i = 0; i < len; i++) m[off++] = val[i];
    return off;
}

/* Start a client message: fixed header, cookie, and the message type option. Returns
 * the offset where the next option goes. */
static int start_message(uint8_t *m, uint32_t xid, uint8_t type) {
    for (int i = 0; i < DHCP_OPTIONS + 64; i++) m[i] = 0;
    m[0] = 1;                                   /* BOOTREQUEST */
    m[1] = 1;                                   /* Ethernet */
    m[2] = 6;                                   /* MAC length */
    m[4] = (uint8_t)(xid >> 24);
    m[5] = (uint8_t)(xid >> 16);
    m[6] = (uint8_t)(xid >> 8);
    m[7] = (uint8_t)xid;
    net_put16(m + 10, 0x8000);                  /* ask for the reply by broadcast */
    const uint8_t *mac = net_mac();
    for (int i = 0; i < 6; i++) m[28 + i] = mac[i];
    for (int i = 0; i < 4; i++) m[DHCP_HDR + i] = k_magic[i];
    uint8_t t = type;
    return put_option(m, DHCP_OPTIONS, OPT_MSG_TYPE, 1, &t);
}

/* Close the option list and pad to the minimum length. Returns the message length. */
static int finish_message(uint8_t *m, int off) {
    m[off++] = OPT_END;
    while (off < DHCP_MIN_LEN) m[off++] = OPT_PAD;
    return off;
}

/* Ask for the mask, gateway and DNS server in the reply. */
static int add_param_list(uint8_t *m, int off) {
    static const uint8_t params[3] = {OPT_SUBNET, OPT_ROUTER, OPT_DNS};
    return put_option(m, off, OPT_PARAM_LIST, 3, params);
}

/* What we keep from a reply's options. */
typedef struct {
    uint8_t msg_type;
    uint8_t server[4];
    uint8_t mask[4];
    uint8_t router[4];
    uint8_t dns[4];
    uint32_t lease_seconds;
    bool has_mask, has_router, has_dns, has_server;
} reply_t;

/* Read the options of a reply. Returns false if the cookie is missing. */
static bool read_options(const uint8_t *m, uint16_t len, reply_t *r) {
    if (len < DHCP_OPTIONS) return false;
    for (int i = 0; i < 4; i++) {
        if (m[DHCP_HDR + i] != k_magic[i]) return false;
    }
    int off = DHCP_OPTIONS;
    while (off < (int)len) {
        uint8_t code = m[off];
        if (code == OPT_END) break;
        if (code == OPT_PAD) {
            off++;
            continue;
        }
        if (off + 1 >= (int)len) break;
        uint8_t olen = m[off + 1];
        if (off + 2 + olen > (int)len) break;
        const uint8_t *v = m + off + 2;
        switch (code) {
        case OPT_MSG_TYPE:
            if (olen >= 1) r->msg_type = v[0];
            break;
        case OPT_SERVER_ID:
            if (olen >= 4) { for (int k = 0; k < 4; k++) r->server[k] = v[k]; r->has_server = true; }
            break;
        case OPT_SUBNET:
            if (olen >= 4) { for (int k = 0; k < 4; k++) r->mask[k] = v[k]; r->has_mask = true; }
            break;
        case OPT_ROUTER:
            if (olen >= 4) { for (int k = 0; k < 4; k++) r->router[k] = v[k]; r->has_router = true; }
            break;
        case OPT_DNS:
            if (olen >= 4) { for (int k = 0; k < 4; k++) r->dns[k] = v[k]; r->has_dns = true; }
            break;
        case OPT_LEASE_TIME:
            if (olen >= 4) {
                r->lease_seconds = ((uint32_t)v[0] << 24) | ((uint32_t)v[1] << 16) |
                                   ((uint32_t)v[2] << 8) | (uint32_t)v[3];
            }
            break;
        default:
            break;
        }
        off += 2 + olen;
    }
    return true;
}

/* Wait for a reply to our xid with the wanted message type. Fills `r` and `yiaddr`.
 * Returns true when one arrives before the timeout. */
static bool wait_reply(int slot, uint32_t xid, uint8_t want, uint32_t timeout_ms,
                       reply_t *r, uint8_t yiaddr[4]) {
    static uint8_t buf[DHCP_MSG_MAX];
    uint64_t start = time_uptime_ms();
    for (;;) {
        uint64_t elapsed = time_uptime_ms() - start;
        if (elapsed >= timeout_ms) return false;
        uint8_t src[4];
        uint16_t sport = 0;
        int got = net_udp_recv(slot, src, &sport, buf, DHCP_MSG_MAX, (uint32_t)(timeout_ms - elapsed));
        if (got < DHCP_OPTIONS || got > DHCP_MSG_MAX) continue;
        if (sport != DHCP_SERVER_PORT) continue;
        if (buf[0] != 2) continue;                              /* BOOTREPLY only */
        uint32_t got_xid = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) |
                           ((uint32_t)buf[6] << 8) | (uint32_t)buf[7];
        if (got_xid != xid) continue;                           /* another client's reply */
        reply_t tmp = {0};
        if (!read_options(buf, (uint16_t)got, &tmp)) continue;
        if (tmp.msg_type != want) continue;
        *r = tmp;
        for (int k = 0; k < 4; k++) yiaddr[k] = buf[16 + k];
        return true;
    }
}

bool net_dhcp_configure(uint32_t timeout_ms, dhcp_lease_t *out) {
    static uint8_t msg[DHCP_MSG_MAX];
    if (!net_ready()) return false;
    int slot = net_udp_bind(DHCP_CLIENT_PORT);
    if (slot < 0) return false;

    uint32_t xid = (uint32_t)time_tsc() ^ 0x48454C50u;     /* a new id per exchange */
    bool ok = false;
    reply_t offer = {0};
    uint8_t offered[4] = {0, 0, 0, 0};

    /* DISCOVER, broadcast from 0.0.0.0. Re-sent up to three times if no offer comes. */
    uint32_t each = timeout_ms / 3;
    if (each == 0) each = 1;
    for (int attempt = 0; attempt < 3 && !ok; attempt++) {
        int off = start_message(msg, xid, MSG_DISCOVER);
        off = add_param_list(msg, off);
        int n = finish_message(msg, off);
        if (!net_udp_send_from(k_zero, k_broadcast, DHCP_SERVER_PORT, DHCP_CLIENT_PORT, msg, (uint16_t)n)) break;
        if (wait_reply(slot, xid, MSG_OFFER, each, &offer, offered)) ok = true;
    }
    if (!ok) {
        klog_note(KLOG_INPUT, "[net] DHCP: no offer\n");
        net_udp_close(slot);
        return false;
    }

    /* REQUEST the offered address from the server that made the offer. */
    reply_t ack = {0};
    uint8_t acked[4];
    bool got_ack = false;
    for (int attempt = 0; attempt < 2 && !got_ack; attempt++) {
        int off = start_message(msg, xid, MSG_REQUEST);
        off = put_option(msg, off, OPT_REQUESTED_IP, 4, offered);
        if (offer.has_server) off = put_option(msg, off, OPT_SERVER_ID, 4, offer.server);
        off = add_param_list(msg, off);
        int n = finish_message(msg, off);
        if (!net_udp_send_from(k_zero, k_broadcast, DHCP_SERVER_PORT, DHCP_CLIENT_PORT, msg, (uint16_t)n)) break;
        if (wait_reply(slot, xid, MSG_ACK, each, &ack, acked)) got_ack = true;
    }
    net_udp_close(slot);
    if (!got_ack) {
        klog_note(KLOG_INPUT, "[net] DHCP: no ACK\n");
        return false;
    }

    /* Missing options fall back to what a /24 with the server as router needs. */
    static const uint8_t default_mask[4] = {255, 255, 255, 0};
    const uint8_t *mask = ack.has_mask ? ack.mask : default_mask;
    const uint8_t *router = ack.has_router ? ack.router : (ack.has_server ? ack.server : net_gateway());
    const uint8_t *dns = ack.has_dns ? ack.dns : (ack.has_server ? ack.server : net_dns_server());
    net_set_config(acked, mask, router, dns);

    for (int k = 0; k < 4; k++) {
        out->ip[k] = acked[k];
        out->mask[k] = mask[k];
        out->router[k] = router[k];
        out->dns[k] = dns[k];
        out->server[k] = ack.has_server ? ack.server[k] : 0;
    }
    out->lease_seconds = ack.lease_seconds;
    klog_note(KLOG_INPUT, "[net] DHCP lease %u.%u.%u.%u\n", acked[0], acked[1], acked[2], acked[3]);
    return true;
}
