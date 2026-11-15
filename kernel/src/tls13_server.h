/* tls13_server.h — parses the server's ServerHello (RFC 8446 section 4.1.3) and
 * rejects anything this client does not support. */
#ifndef HELIX_TLS13_SERVER_H
#define HELIX_TLS13_SERVER_H

#include <stddef.h>
#include <stdint.h>

/* Takes one handshake message (4-byte header included). Checks the echoed
 * session id, the chosen suite, the TLS 1.3 version and the x25519 key share.
 * Returns true and writes the server's public key when all match. A
 * HelloRetryRequest is refused, since this client sends one key share only. */
int tls13_parse_server_hello(const uint8_t *msg, size_t len,
                             const uint8_t session_id[32],
                             uint8_t server_pub[32]);

#endif /* HELIX_TLS13_SERVER_H */
