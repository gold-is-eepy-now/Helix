/* net.h — the kernel network stack: one interface, polled, IPv4 with ARP and ICMP echo.
 *
 * The layers, bottom up (see docs/10-network.md):
 *   e1000.c    raw Ethernet frames in and out
 *   net_eth.c  Ethernet header build
 *   net_arp.c  IPv4-to-MAC resolution with a small cache
 *   net_ip.c   IPv4 header build, checksum, and routing to the next hop
 *   net_icmp.c ICMP echo request and reply (ping)
 *
 * Nothing runs in the background. net_poll() drains the receive ring and hands each
 * frame up the stack; a waiting call (net_ping, net_arp_resolve) calls it in a loop. */
#ifndef HELIX_NET_H
#define HELIX_NET_H

#include <stdbool.h>
#include <stdint.h>

/* Largest Ethernet frame we handle (FCS excluded). */
#define NET_MAX_FRAME 1518u

/* Read the MAC from the card and set the static address (10.0.2.15/24, gateway
 * 10.0.2.2: QEMU's user-mode network). Does nothing when no card is present. */
void net_init(void);
bool net_ready(void);

const uint8_t *net_mac(void);
const uint8_t *net_ip_addr(void);
const uint8_t *net_gateway(void);
const uint8_t *net_netmask(void);

/* Drain the receive ring and dispatch each frame by EtherType. */
void net_poll(void);

/* Parse "a.b.c.d" into out. False if the text is not a dotted quad. */
bool net_parse_ip(const char *text, uint8_t out[4]);

/* Big-endian (network order) access for header fields. */
static inline uint16_t net_get16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}
static inline void net_put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

#endif /* HELIX_NET_H */
