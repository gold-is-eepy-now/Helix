/* net_tls.h — a TLS 1.3 session over an established TCP connection (net_tcp). Used by
 * the HTTPS client (net_http.c). Opening a session does the whole handshake and checks
 * the server's certificate chain and signature (tls13_verify.c, x509_verify.c) before
 * any application data is sent. If any check fails, the session is never returned.
 *
 * Cipher suite: TLS_CHACHA20_POLY1305_SHA256 with X25519 only. Nothing else is offered. */
#ifndef HELIX_NET_TLS_H
#define HELIX_NET_TLS_H

#include <stddef.h>
#include <stdint.h>

typedef struct net_tls net_tls_t;

/* Handshake over `conn` (a connected net_tcp socket) for `host`, which is also the name
 * the certificate must cover. On success returns a session; on failure returns NULL and
 * writes the reason into err. The caller still owns `conn` and closes it after
 * net_tls_close. */
net_tls_t *net_tls_open(int conn, const char *host, char *err, size_t err_cap);

/* Encrypts and sends len bytes. Returns 0 on success, -1 on a network error. */
int net_tls_send(net_tls_t *t, int conn, const uint8_t *data, uint32_t len);

/* Reads decrypted application data into buf. Returns the number of bytes (at least 1),
 * 0 at a clean end (close_notify or TCP close), or -1 on error or timeout. */
int net_tls_recv(net_tls_t *t, int conn, uint8_t *buf, uint32_t cap);

/* Frees the session. Does not close the TCP connection. */
void net_tls_close(net_tls_t *t);

#endif /* HELIX_NET_TLS_H */
