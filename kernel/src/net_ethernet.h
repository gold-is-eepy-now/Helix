/* net_ethernet.h — Ethernet II framing: a 14-byte header in front of each payload.
 *
 * Header layout (each field big-endian where it is more than one byte):
 *   [0..5]   destination MAC   [6..11] source MAC   [12..13] EtherType
 */
#ifndef HELIX_NET_ETHERNET_H
#define HELIX_NET_ETHERNET_H

#include <stdbool.h>
#include <stdint.h>

#define NET_ETH_HLEN        14
#define NET_ETHERTYPE_IPV4  0x0800
#define NET_ETHERTYPE_ARP   0x0806

/* Build a frame (our MAC as the source) and hand it to the card. False if the
 * payload would not fit in one frame or the card refuses it. */
bool net_eth_send(const uint8_t dst[6], uint16_t ethertype,
                  const uint8_t *payload, uint16_t length);

#endif /* HELIX_NET_ETHERNET_H */
