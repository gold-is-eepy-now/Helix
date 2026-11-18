/* hkdf.h — HKDF-SHA256 (RFC 5869): extract and expand. TLS 1.3 derives all of
 * its keys with these two steps. Built on hmac_sha256 from sha256.h. */
#ifndef HELIX_HKDF_H
#define HELIX_HKDF_H

#include <stddef.h>
#include <stdint.h>

#include "sha256.h"

/* PRK = HMAC(salt, ikm). An empty salt is treated as 32 zero bytes, which
 * HMAC's zero-padding already does. */
void hkdf_extract(uint8_t prk[SHA256_DIGEST_LEN],
                  const uint8_t *salt, size_t salt_len,
                  const uint8_t *ikm, size_t ikm_len);

/* Fills out with out_len bytes. out_len must be at most 255 * 32. */
void hkdf_expand(uint8_t *out, size_t out_len,
                 const uint8_t prk[SHA256_DIGEST_LEN],
                 const uint8_t *info, size_t info_len);

#endif /* HELIX_HKDF_H */
