/* tls13_schedule.h — the TLS 1.3 key schedule (RFC 8446 section 7.1) for the
 * SHA-256 hash: HKDF-Expand-Label, Derive-Secret, and traffic keys. */
#ifndef HELIX_TLS13_SCHEDULE_H
#define HELIX_TLS13_SCHEDULE_H

#include <stddef.h>
#include <stdint.h>

#include "sha256.h"

/* HKDF-Expand-Label(secret, label, context, len): label is given without the
 * "tls13 " prefix. */
void tls13_expand_label(uint8_t *out, size_t len, const uint8_t secret[SHA256_DIGEST_LEN],
                        const char *label, const uint8_t *context, size_t context_len);

/* Derive-Secret(secret, label, messages): transcript_hash is Hash(messages). */
void tls13_derive_secret(uint8_t out[SHA256_DIGEST_LEN], const uint8_t secret[SHA256_DIGEST_LEN],
                         const char *label, const uint8_t transcript_hash[SHA256_DIGEST_LEN]);

/* Traffic key and IV for a record protection key: key_len bytes and 12-byte IV. */
void tls13_traffic_keys(uint8_t *key, size_t key_len, uint8_t iv[12],
                        const uint8_t traffic_secret[SHA256_DIGEST_LEN]);

#endif /* HELIX_TLS13_SCHEDULE_H */
