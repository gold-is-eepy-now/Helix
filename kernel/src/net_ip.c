/* net_ip.c — IPv4 packets (see net_ip.h).
 *
 * IPv4 header, the 20 bytes we send (no options):
 *   0 version/IHL (0x45)   1 TOS   2 total length   4 identification
 *   6 flags/fragment offset (we set Don't Fragment)   8 TTL   9 protocol
 *   10 header checksum   12 source IP   16 destination IP
 */
#include "net_ip.h"

#include <stddef.h>

#include "klog.h"
#include "net.h"
#include "net_arp.h"
#include "net_ethernet.h"
#include "net_icmp.h"

#define IP_HLEN      20
#define IP_TTL       64
#define IP_FLAG_DF   0x4000
#define IP_FRAG_MASK 0x3FFF     /* more-fragments flag plus fragment offset */

uint16_t net_checksum(const uint8_t *data, uint16_t len) {
    uint32_t sum = 0;
    uint16_t i = 0;
    for (; i + 1 < len; i += 2) sum += net_get16(data + i);
    if (i < len) sum += (uint32_t)data[i] << 8;      /* odd byte: pad with zero */
    while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);   /* fold the carries */
    return (uint16_t)~sum;
}

static bool same4(const uint8_t *a, const uint8_t *b) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

/* True if `ip` shares our network prefix (the bits covered by the netmask). */
static bool on_subnet(const uint8_t *ip) {
    const uint8_t *mine = net_ip_addr();
    const uint8_t *mask = net_netmask();
    for (int i = 0; i < 4; i++) {
        if ((ip[i] & mask[i]) != (mine[i] & mask[i])) return false;
    }
    return true;
}

bool net_ip_send(const uint8_t dst[4], uint8_t proto, const uint8_t *payload, uint16_t len) {
    /* Static: a packet is too big for the kernel stack. The next hop is resolved
     * before the buffer is filled, so a nested send during resolution cannot clash. */
    static uint8_t pkt[NET_MAX_FRAME - NET_ETH_HLEN];
    static uint16_t next_id;
    if (!net_ready()) return false;
    if (len > sizeof pkt - IP_HLEN) return false;

    const uint8_t *hop = on_subnet(dst) ? dst : net_gateway();
    uint8_t mac[6];
    if (!net_arp_resolve(hop, mac, 1000)) {
        klog_note(KLOG_INPUT, "[net] no ARP reply from %u.%u.%u.%u\n", hop[0], hop[1], hop[2], hop[3]);
        return false;
    }

    uint16_t total = (uint16_t)(IP_HLEN + len);
    pkt[0] = 0x45;                          /* IPv4, five 32-bit words (no options) */
    pkt[1] = 0;
    net_put16(pkt + 2, total);
    net_put16(pkt + 4, next_id++);
    net_put16(pkt + 6, IP_FLAG_DF);
    pkt[8] = IP_TTL;
    pkt[9] = proto;
    net_put16(pkt + 10, 0);                 /* checksum is zero while it is computed */
    const uint8_t *src = net_ip_addr();
    for (int i = 0; i < 4; i++) {
        pkt[12 + i] = src[i];
        pkt[16 + i] = dst[i];
    }
    net_put16(pkt + 10, net_checksum(pkt, IP_HLEN));
    for (uint16_t i = 0; i < len; i++) pkt[IP_HLEN + i] = payload[i];

    return net_eth_send(mac, NET_ETHERTYPE_IPV4, pkt, total);
}

void net_ip_input(const uint8_t *frame, uint16_t n) {
    if (n < NET_ETH_HLEN + IP_HLEN) return;
    const uint8_t *ip = frame + NET_ETH_HLEN;
    uint16_t avail = (uint16_t)(n - NET_ETH_HLEN);
    if ((ip[0] >> 4) != 4) return;                       /* not IPv4 */
    uint16_t hl = (uint16_t)((ip[0] & 0x0F) * 4);        /* header length in bytes */
    uint16_t total = net_get16(ip + 2);
    if (hl < IP_HLEN || hl > avail || total < hl || total > avail) return;
    if (net_checksum(ip, hl) != 0) {
        klog_note(KLOG_INPUT, "[net] bad IPv4 header checksum, dropped\n");
        return;
    }
    static const uint8_t broadcast[4] = {255, 255, 255, 255};
    if (!same4(ip + 16, net_ip_addr()) && !same4(ip + 16, broadcast)) return;   /* not for us */
    if (net_get16(ip + 6) & IP_FRAG_MASK) {
        /* Fragments are not reassembled yet (docs/10-network.md: no fragmentation). */
        klog_note(KLOG_INPUT, "[net] IPv4 fragment dropped\n");
        return;
    }
    if (ip[9] == NET_IP_PROTO_ICMP) {
        net_icmp_input(ip + 12, ip + hl, (uint16_t)(total - hl));
    }
}
