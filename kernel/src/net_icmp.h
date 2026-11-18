/* net_icmp.h — ICMP echo: answer echo requests, and send our own (ping).
 *
 * An echo request carries an identifier and a sequence number; the reply copies both
 * and the payload back. We match replies on identifier, sequence and sender. */
#ifndef HELIX_NET_ICMP_H
#define HELIX_NET_ICMP_H

#include <stdbool.h>
#include <stdint.h>

/* Handle one ICMP message from `src` (the IPv4 payload, length len). */
void net_icmp_input(const uint8_t src[4], const uint8_t *msg, uint16_t len);

/* Send one echo request to `dst` and wait for its reply. True and the round-trip
 * time in microseconds (from the TSC) on success; false on timeout or if the send fails. */
bool net_ping(const uint8_t dst[4], uint16_t seq, uint32_t timeout_ms, uint32_t *rtt_us);

#endif /* HELIX_NET_ICMP_H */
