/* tls13.h — the TLS 1.3 key schedule (RFC 8446 section 7.1) for the
 * SHA-256 / ChaCha20-Poly1305 suite. The transcript hash is an input here:
 * the handshake code computes it from the messages it has sent and received. */
#ifndef HELIX_TLS13_H
#define HELIX_TLS13_H

#include <stddef.h>
#include <stdint.h>

#include "sha256.h"

/* HKDF-Expand-Label(secret, label, context, length) from RFC 8446 7.1. */
void tls13_expand_label(uint8_t *out, size_t out_len,
                        const uint8_t secret[SHA256_DIGEST_LEN],
                        const char *label,
                        const uint8_t *context, size_t context_len);

/* Derive-Secret(secret, label, messages) = Expand-Label(secret, label, Hash(messages), 32). */
void tls13_derive_secret(uint8_t out[SHA256_DIGEST_LEN],
                         const uint8_t secret[SHA256_DIGEST_LEN],
                         const char *label,
                         const uint8_t transcript_hash[SHA256_DIGEST_LEN]);

/* Traffic key (16 bytes) and IV (12 bytes) for ChaCha20-Poly1305. */
void tls13_traffic_key_iv(uint8_t key[16], uint8_t iv[12],
                          const uint8_t traffic_secret[SHA256_DIGEST_LEN]);

/* The three secrets of the schedule. Each is an HKDF-Extract whose IKM is the
 * previous stage's material (zeros when there is no PSK or no key exchange yet). */
void tls13_early_secret(uint8_t out[SHA256_DIGEST_LEN]);
void tls13_handshake_secret(uint8_t out[SHA256_DIGEST_LEN],
                            const uint8_t early[SHA256_DIGEST_LEN],
                            const uint8_t ecdhe[32]);
void tls13_master_secret(uint8_t out[SHA256_DIGEST_LEN],
                         const uint8_t handshake[SHA256_DIGEST_LEN]);

#endif /* HELIX_TLS13_H */
