/* net_udp.c — UDP over IPv4 (see net_udp.h).
 *
 * UDP header, 8 bytes:
 *   0 source port   2 destination port   4 length (header + data)   6 checksum (0 = none)
 */
#include "net_udp.h"

#include <stddef.h>

#include "klog.h"
#include "net.h"
#include "net_ip.h"
#include "time.h"

#define UDP_HLEN         8
#define UDP_QUEUE        4        /* datagrams held per port before new ones are dropped */
#define UDP_MAX_PAYLOAD  512      /* largest datagram we keep; longer ones are dropped */

typedef struct {
    uint16_t len;
    uint16_t src_port;
    uint8_t src_ip[4];
    uint8_t data[UDP_MAX_PAYLOAD];
} udp_dgram_t;

typedef struct {
    bool bound;
    uint16_t port;
    uint8_t head;                 /* index of the oldest queued datagram */
    uint8_t count;                /* datagrams queued */
    udp_dgram_t q[UDP_QUEUE];
} udp_slot_t;

static udp_slot_t g_slots[NET_UDP_SLOTS];

int net_udp_bind(uint16_t port) {
    int free_slot = -1;
    for (int i = 0; i < NET_UDP_SLOTS; i++) {
        if (g_slots[i].bound) {
            if (g_slots[i].port == port) return -1;
        } else if (free_slot < 0) {
            free_slot = i;
        }
    }
    if (free_slot < 0) return -1;
    g_slots[free_slot].bound = true;
    g_slots[free_slot].port = port;
    g_slots[free_slot].head = 0;
    g_slots[free_slot].count = 0;
    return free_slot;
}

void net_udp_close(int slot) {
    if (slot < 0 || slot >= NET_UDP_SLOTS) return;
    g_slots[slot].bound = false;
    g_slots[slot].count = 0;
}

bool net_udp_send_from(const uint8_t src[4], const uint8_t dst[4], uint16_t dst_port,
                       uint16_t src_port, const uint8_t *data, uint16_t len) {
    /* Static: the segment is too big for the kernel stack. Only this function fills it. */
    static uint8_t seg[UDP_HLEN + UDP_MAX_PAYLOAD];
    if (len > UDP_MAX_PAYLOAD) return false;
    net_put16(seg, src_port);
    net_put16(seg + 2, dst_port);
    net_put16(seg + 4, (uint16_t)(UDP_HLEN + len));
    net_put16(seg + 6, 0);
    for (uint16_t i = 0; i < len; i++) seg[UDP_HLEN + i] = data[i];
    return net_ip_send_from(src, dst, NET_IP_PROTO_UDP, seg, (uint16_t)(UDP_HLEN + len));
}

bool net_udp_send(const uint8_t dst[4], uint16_t dst_port, uint16_t src_port,
                  const uint8_t *data, uint16_t len) {
    return net_udp_send_from(net_ip_addr(), dst, dst_port, src_port, data, len);
}

void net_udp_input(const uint8_t src[4], const uint8_t *seg, uint16_t len) {
    if (len < UDP_HLEN) return;
    uint16_t ulen = net_get16(seg + 4);
    if (ulen < UDP_HLEN || ulen > len) return;      /* length field lies: drop */
    uint16_t sport = net_get16(seg);
    uint16_t dport = net_get16(seg + 2);
    for (int i = 0; i < NET_UDP_SLOTS; i++) {
        if (!g_slots[i].bound || g_slots[i].port != dport) continue;
        udp_slot_t *s = &g_slots[i];
        if (s->count >= UDP_QUEUE) {
            klog_note(KLOG_INPUT, "[net] UDP queue full on port %u, datagram dropped\n", dport);
            return;
        }
        udp_dgram_t *d = &s->q[(s->head + s->count) % UDP_QUEUE];
        uint16_t n = (uint16_t)(ulen - UDP_HLEN);
        if (n > UDP_MAX_PAYLOAD) n = UDP_MAX_PAYLOAD;
        d->len = n;
        d->src_port = sport;
        for (int k = 0; k < 4; k++) d->src_ip[k] = src[k];
        for (uint16_t k = 0; k < n; k++) d->data[k] = seg[UDP_HLEN + k];
        s->count++;
        return;
    }
    /* No socket on that port: a real stack would send ICMP port unreachable. Not yet. */
}

int net_udp_recv(int slot, uint8_t src_ip[4], uint16_t *src_port, uint8_t *buf,
                 uint16_t cap, uint32_t timeout_ms) {
    if (slot < 0 || slot >= NET_UDP_SLOTS || !g_slots[slot].bound) return -1;
    udp_slot_t *s = &g_slots[slot];
    uint64_t start = time_uptime_ms();
    for (;;) {
        if (s->count > 0) {
            udp_dgram_t *d = &s->q[s->head];
            uint16_t n = d->len < cap ? d->len : cap;
            for (uint16_t k = 0; k < n; k++) buf[k] = d->data[k];
            for (int k = 0; k < 4; k++) src_ip[k] = d->src_ip[k];
            *src_port = d->src_port;
            int total = d->len;
            s->head = (uint8_t)((s->head + 1) % UDP_QUEUE);
            s->count--;
            return total;
        }
        net_poll();
        if (s->count > 0) continue;
        if (time_uptime_ms() - start >= timeout_ms) return -1;
    }
}
