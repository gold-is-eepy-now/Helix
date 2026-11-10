/* net_ip.h — IPv4: header build and parse, the Internet checksum, and routing.
 *
 * Routing is two rules: a destination on our subnet is sent to itself; anything
 * else goes to the gateway. The next hop's MAC comes from net_arp_resolve. */
#ifndef HELIX_NET_IP_H
#define HELIX_NET_IP_H

#include <stdbool.h>
#include <stdint.h>

#define NET_IP_PROTO_ICMP 1

/* The Internet checksum: one's-complement sum of 16-bit big-endian words, inverted.
 * Run over a header that already holds its checksum, the result is 0. */
uint16_t net_checksum(const uint8_t *data, uint16_t len);

/* Send `len` bytes of `payload` to `dst` as protocol `proto`. False if the next hop
 * cannot be resolved, the packet is too big, or the card refuses it. */
bool net_ip_send(const uint8_t dst[4], uint8_t proto, const uint8_t *payload, uint16_t len);

/* Handle one received Ethernet frame (length n) that carries IPv4. */
void net_ip_input(const uint8_t *frame, uint16_t n);

#endif /* HELIX_NET_IP_H */
