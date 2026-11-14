/* net_tcp.h — TCP connections: connect or listen, send, receive, close.
 *
 * Scope: active opens (connect) and passive opens (listen, accept, one port each).
 * Data is delivered in order. Out-of-order segments are dropped and re-sent by the peer.
 * Sending is stop-and-wait per window with go-back-N retransmission after a timeout.
 * Options: MSS is sent in the SYN; nothing else (no window scaling, SACK or timestamps).
 * See docs/10-network.md. */
#ifndef HELIX_NET_TCP_H
#define HELIX_NET_TCP_H

#include <stdint.h>

#define NET_TCP_CONNS 4
#define NET_TCP_LISTENERS 4

/* Start listening on a local port. Returns a listener id, or -1 if the port is already
 * listening or no listener slot is free. Connections share the NET_TCP_CONNS slots. */
int net_tcp_listen(uint16_t port);

/* Wait up to timeout_ms for a peer to finish connecting to the listener. Returns a
 * connection id (use it with send, recv and close), or -1 on timeout. Handshakes that
 * never finish are dropped after a few seconds. Blocks, polling the network. */
int net_tcp_accept(int listener, uint32_t timeout_ms);

/* Stop listening. Connections already accepted are not affected. */
void net_tcp_unlisten(int listener);

/* Open a connection to ip:port. Returns a connection id, or -1 (no free slot, timeout,
 * or refused). Blocks, polling the network, until established or timeout_ms passes. */
int net_tcp_connect(const uint8_t ip[4], uint16_t port, uint32_t timeout_ms);

/* Send all of data. Returns len, or -1 if the peer stopped acknowledging. Blocks. */
int net_tcp_send(int conn, const uint8_t *data, uint32_t len, uint32_t timeout_ms);

/* Receive up to cap bytes. Returns the count (at least 1), 0 when the peer has closed
 * and everything it sent has been read, or -1 on timeout or reset. */
int net_tcp_recv(int conn, uint8_t *buf, uint32_t cap, uint32_t timeout_ms);

/* Send FIN, wait briefly for the peer's FIN, and free the slot. */
void net_tcp_close(int conn);

/* Called by net_ip_input for each TCP segment. dst is our address. */
void net_tcp_input(const uint8_t src[4], const uint8_t dst[4], const uint8_t *seg, uint16_t len);

#endif /* HELIX_NET_TCP_H */
