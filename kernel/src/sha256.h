/* sha256.h — SHA-256 (FIPS 180-4), HMAC-SHA256 (RFC 2104) and PBKDF2-HMAC-SHA256
 * (RFC 8018). Integer code only: no floating point, no SIMD, so it runs in the
 * kernel as it is. Used for password hashing in accounts.c. */
#ifndef HELIX_SHA256_H
#define HELIX_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define SHA256_DIGEST_LEN 32u

typedef struct {
    uint32_t h[8];
    uint8_t buf[64];
    uint64_t len;      /* bytes absorbed so far */
    uint32_t blen;     /* bytes waiting in buf */
} sha256_ctx_t;

void sha256_init(sha256_ctx_t *c);
void sha256_update(sha256_ctx_t *c, const void *data, size_t n);
void sha256_final(sha256_ctx_t *c, uint8_t out[SHA256_DIGEST_LEN]);
void sha256(const void *data, size_t n, uint8_t out[SHA256_DIGEST_LEN]);

void hmac_sha256(const uint8_t *key, size_t key_len,
                 const uint8_t *msg, size_t msg_len,
                 uint8_t out[SHA256_DIGEST_LEN]);

/* salt_len must be at most 64. out_len may be any size. */
void pbkdf2_sha256(const uint8_t *password, size_t password_len,
                   const uint8_t *salt, size_t salt_len,
                   uint32_t iterations, uint8_t *out, size_t out_len);

#endif /* HELIX_SHA256_H */
