/* net_arp.c — ARP request, reply, and the resolution cache (see net_arp.h).
 *
 * ARP packet (28 bytes for Ethernet/IPv4), offsets from the start of the packet:
 *   0 hardware type (1)   2 protocol type (0x0800)   4 hw length (6)   5 proto length (4)
 *   6 operation (1 = request, 2 = reply)
 *   8 sender MAC   14 sender IP   18 target MAC   24 target IP
 */
#include "net_arp.h"

#include <stddef.h>

#include "net.h"
#include "net_ethernet.h"
#include "time.h"

#define ARP_LEN       28
#define ARP_CACHE     16
#define ARP_RETRY_MS  250

typedef struct {
    uint8_t ip[4];
    uint8_t mac[6];
    bool valid;
} arp_entry_t;

/* Round-robin replacement: when full, the oldest slot is reused. Good enough for a
 * handful of hosts; there is no aging yet (see docs/10-network.md). */
static arp_entry_t g_cache[ARP_CACHE];
static uint8_t g_next;

static const uint8_t k_broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static const uint8_t k_zero_mac[6] = {0, 0, 0, 0, 0, 0};

static bool same4(const uint8_t *a, const uint8_t *b) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

static bool is_zero4(const uint8_t *a) {
    return a[0] == 0 && a[1] == 0 && a[2] == 0 && a[3] == 0;
}

static void learn(const uint8_t ip[4], const uint8_t mac[6]) {
    for (int i = 0; i < ARP_CACHE; i++) {
        if (g_cache[i].valid && same4(g_cache[i].ip, ip)) {
            for (int k = 0; k < 6; k++) g_cache[i].mac[k] = mac[k];
            return;
        }
    }
    arp_entry_t *slot = &g_cache[g_next];
    g_next = (uint8_t)((g_next + 1) % ARP_CACHE);
    for (int k = 0; k < 4; k++) slot->ip[k] = ip[k];
    for (int k = 0; k < 6; k++) slot->mac[k] = mac[k];
    slot->valid = true;
}

bool net_arp_lookup(const uint8_t ip[4], uint8_t mac[6]) {
    for (int i = 0; i < ARP_CACHE; i++) {
        if (g_cache[i].valid && same4(g_cache[i].ip, ip)) {
            for (int k = 0; k < 6; k++) mac[k] = g_cache[i].mac[k];
            return true;
        }
    }
    return false;
}

static void fill(uint8_t *p, uint16_t op, const uint8_t *smac, const uint8_t *sip,
                 const uint8_t *tmac, const uint8_t *tip) {
    net_put16(p, 1);                  /* Ethernet */
    net_put16(p + 2, NET_ETHERTYPE_IPV4);
    p[4] = 6;
    p[5] = 4;
    net_put16(p + 6, op);
    for (int i = 0; i < 6; i++) p[8 + i] = smac[i];
    for (int i = 0; i < 4; i++) p[14 + i] = sip[i];
    for (int i = 0; i < 6; i++) p[18 + i] = tmac[i];
    for (int i = 0; i < 4; i++) p[24 + i] = tip[i];
}

void net_arp_request(const uint8_t ip[4]) {
    uint8_t p[ARP_LEN];
    fill(p, 1, net_mac(), net_ip_addr(), k_zero_mac, ip);
    net_eth_send(k_broadcast, NET_ETHERTYPE_ARP, p, ARP_LEN);
}

void net_arp_input(const uint8_t *frame, uint16_t n) {
    if (n < NET_ETH_HLEN + ARP_LEN) return;
    const uint8_t *a = frame + NET_ETH_HLEN;
    /* Only Ethernet/IPv4 ARP: anything else is a protocol we do not speak. */
    if (net_get16(a) != 1 || net_get16(a + 2) != NET_ETHERTYPE_IPV4 || a[4] != 6 || a[5] != 4) {
        return;
    }
    uint16_t op = net_get16(a + 6);
    const uint8_t *sha = a + 8;
    const uint8_t *spa = a + 14;
    const uint8_t *tpa = a + 24;

    /* A sender with 0.0.0.0 is probing for an address, not announcing one. */
    if (!is_zero4(spa)) learn(spa, sha);

    if (op == 1 && same4(tpa, net_ip_addr())) {
        uint8_t p[ARP_LEN];
        fill(p, 2, net_mac(), net_ip_addr(), sha, spa);
        net_eth_send(sha, NET_ETHERTYPE_ARP, p, ARP_LEN);
    }
}

bool net_arp_resolve(const uint8_t ip[4], uint8_t mac[6], uint32_t timeout_ms) {
    if (net_arp_lookup(ip, mac)) return true;
    /* The timeout is measured on the clock; without it we would wait forever. */
    if (!time_available()) return false;

    uint64_t start = time_uptime_ms();
    uint64_t next_request = start;
    for (;;) {
        uint64_t now = time_uptime_ms();
        if (now >= next_request) {
            net_arp_request(ip);
            next_request = now + ARP_RETRY_MS;
        }
        net_poll();
        if (net_arp_lookup(ip, mac)) return true;
        if (now - start >= timeout_ms) return false;
    }
}
