/* tls13_hello.h — the TLS 1.3 ClientHello (RFC 8446 section 4.1.2). Offers only
 * TLS_CHACHA20_POLY1305_SHA256 and the x25519 key share. */
#ifndef HELIX_TLS13_HELLO_H
#define HELIX_TLS13_HELLO_H

#include <stddef.h>
#include <stdint.h>

/* Writes the handshake message (4-byte header included) into out. server_name may
 * be NULL. Returns the length, or 0 if out is too small. */
size_t tls13_build_client_hello(uint8_t *out, size_t cap,
                                const uint8_t random[32],
                                const uint8_t session_id[32],
                                const uint8_t x25519_pub[32],
                                const char *server_name);

#endif /* HELIX_TLS13_HELLO_H */
