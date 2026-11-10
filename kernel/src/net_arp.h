/* net_arp.h — ARP for IPv4 over Ethernet: map an IP address to a MAC address.
 *
 * ARP asks "who has 10.0.2.2?" as a broadcast; the owner answers with its MAC.
 * We learn mappings from every ARP packet we see and keep them in a small cache. */
#ifndef HELIX_NET_ARP_H
#define HELIX_NET_ARP_H

#include <stdbool.h>
#include <stdint.h>

/* Handle one received frame (the whole Ethernet frame, length n). Answers requests
 * for our address and records the sender's mapping. */
void net_arp_input(const uint8_t *frame, uint16_t n);

/* Broadcast a request for `ip`. The reply arrives later through net_poll(). */
void net_arp_request(const uint8_t ip[4]);

/* Cache lookup only. True and the MAC if we already know `ip`. */
bool net_arp_lookup(const uint8_t ip[4], uint8_t mac[6]);

/* Find the MAC for `ip`: from the cache, or by asking and polling until the
 * reply arrives. Re-asks every 250 ms. False after timeout_ms. */
bool net_arp_resolve(const uint8_t ip[4], uint8_t mac[6], uint32_t timeout_ms);

#endif /* HELIX_NET_ARP_H */
