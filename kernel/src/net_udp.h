/* net_udp.h — UDP datagrams: a bounded set of bound ports, each with a short queue.
 *
 * A bound port is a socket slot. Incoming datagrams for it are queued by net_poll and
 * taken with net_udp_recv. A full queue drops the new datagram (UDP has no flow control).
 * Checksums are not computed or checked (0 is legal for UDP over IPv4). */
#ifndef HELIX_NET_UDP_H
#define HELIX_NET_UDP_H

#include <stdbool.h>
#include <stdint.h>

#define NET_UDP_SLOTS 4

/* Bind a local port. Returns the slot number, or -1 if the port is already bound or
 * no slot is free. */
int net_udp_bind(uint16_t port);
void net_udp_close(int slot);

/* Send one datagram. The source address is ours unless net_udp_send_from is used. */
bool net_udp_send(const uint8_t dst[4], uint16_t dst_port, uint16_t src_port,
                  const uint8_t *data, uint16_t len);
bool net_udp_send_from(const uint8_t src[4], const uint8_t dst[4], uint16_t dst_port,
                       uint16_t src_port, const uint8_t *data, uint16_t len);

/* Wait up to timeout_ms for a datagram on `slot` (polling the network). Copies up to
 * cap bytes into buf and returns the datagram's full length, or -1 on timeout. The
 * sender goes into src_ip and src_port. */
int net_udp_recv(int slot, uint8_t src_ip[4], uint16_t *src_port, uint8_t *buf,
                 uint16_t cap, uint32_t timeout_ms);

/* Called by net_ip_input for each UDP packet addressed to us. seg is the UDP datagram. */
void net_udp_input(const uint8_t src[4], const uint8_t *seg, uint16_t len);

#endif /* HELIX_NET_UDP_H */
