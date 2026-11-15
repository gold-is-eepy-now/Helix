/* tls13_client.h — the TLS 1.3 client handshake state machine for
 * TLS_CHACHA20_POLY1305_SHA256 with x25519 (RFC 8446 section 4). Transport-agnostic:
 * the caller feeds received bytes and sends whatever tls13_client_take_output() returns.
 *
 * Certificate and CertificateVerify messages are parsed in order and included in the
 * transcript, but NOT yet verified. Application keys are withheld until the caller
 * calls tls13_client_authenticate(), which the certificate code must do only after
 * the chain and signature checks pass. */
#ifndef HELIX_TLS13_CLIENT_H
#define HELIX_TLS13_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#include "sha256.h"
#include "tls13_record.h"

typedef enum {
    TLS_ST_IDLE = 0,
    TLS_ST_WAIT_SH,     /* ClientHello sent */
    TLS_ST_WAIT_EE,     /* ServerHello accepted, handshake keys in use */
    TLS_ST_WAIT_CERT,
    TLS_ST_WAIT_CV,
    TLS_ST_WAIT_FIN,
    TLS_ST_WAIT_AUTH,   /* server Finished verified; certificate not yet verified */
    TLS_ST_READY,       /* certificate verified: application data may flow */
    TLS_ST_FAILED
} tls13_state_t;

#define TLS_HS_BUF_MAX 16384u   /* largest handshake data buffered (certificates included) */

typedef struct {
    tls13_state_t state;
    uint8_t random[32];
    uint8_t session_id[32];
    uint8_t priv[32];
    uint8_t pub[32];
    uint8_t server_pub[32];
    sha256_ctx_t transcript;
    uint8_t hs_secret[32];
    uint8_t c_hs_secret[32];
    uint8_t s_hs_secret[32];
    tls13_cipher_t hs_tx;   /* client handshake keys */
    tls13_cipher_t hs_rx;   /* server handshake keys */
    tls13_cipher_t app_tx;
    tls13_cipher_t app_rx;
    uint8_t hs_buf[TLS_HS_BUF_MAX];
    size_t hs_len;
    uint8_t pending[96];    /* bytes waiting to be sent (the client Finished record) */
    size_t pending_len;
} tls13_client_t;

void tls13_client_init(tls13_client_t *c, const uint8_t random[32],
                       const uint8_t session_id[32], const uint8_t priv[32]);

/* Writes the ClientHello record (host may be NULL). Returns its length or 0. */
size_t tls13_client_hello(tls13_client_t *c, uint8_t *out, size_t cap, const char *host);

/* Consumes whole records from in and returns how many bytes were used; an incomplete
 * record at the end is left for the caller to resend with more data. Returns
 * (size_t)-1 on any protocol error, and the state becomes TLS_ST_FAILED. */
size_t tls13_client_feed(tls13_client_t *c, const uint8_t *in, size_t len);

/* Copies out bytes the client must send (the client Finished). Returns the count. */
size_t tls13_client_take_output(tls13_client_t *c, uint8_t *out, size_t cap);

/* Called by certificate verification after the chain and signature checks pass. */
int tls13_client_authenticate(tls13_client_t *c);

/* Application traffic keys. Returns 0 until the client is authenticated (READY). */
int tls13_client_app_keys(const tls13_client_t *c, tls13_cipher_t *tx, tls13_cipher_t *rx);

#endif /* HELIX_TLS13_CLIENT_H */
