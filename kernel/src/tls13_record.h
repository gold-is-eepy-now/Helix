/* tls13_record.h — TLS 1.3 record protection for TLS_CHACHA20_POLY1305_SHA256
 * (RFC 8446 section 5.2 and 5.3). A cipher state holds one direction's key,
 * IV and record sequence number. */
#ifndef HELIX_TLS13_RECORD_H
#define HELIX_TLS13_RECORD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TLS_CT_CHANGE_CIPHER 20u
#define TLS_CT_ALERT         21u
#define TLS_CT_HANDSHAKE     22u
#define TLS_CT_APPDATA       23u
#define TLS_RECORD_HDR_LEN   5u
#define TLS_TAG_LEN          16u
#define TLS_MAX_PLAIN        16384u

typedef struct {
    uint8_t key[32];
    uint8_t iv[12];
    uint64_t seq;       /* per-direction record counter */
} tls13_cipher_t;

/* nonce = iv XOR (64-bit big-endian sequence number, left-padded to 12 bytes). */
void tls13_nonce(uint8_t nonce[12], const uint8_t iv[12], uint64_t seq);

/* Writes one encrypted record into out (room: plain_len + 1 + 16 + 5 bytes).
 * Returns the total record length. The inner content type is appended to the
 * plaintext before encryption, as TLS 1.3 requires. */
size_t tls13_seal(uint8_t *out, tls13_cipher_t *c, uint8_t content_type,
                  const uint8_t *plain, size_t plain_len);

/* Opens one encrypted record of total length rec_len. On success writes the
 * plaintext (at most rec_len bytes) and the inner content type, and advances
 * the sequence number. A bad tag returns false and changes nothing. */
bool tls13_open(uint8_t *plain, size_t *plain_len, uint8_t *content_type,
                tls13_cipher_t *c, const uint8_t *rec, size_t rec_len);

#endif /* HELIX_TLS13_RECORD_H */
