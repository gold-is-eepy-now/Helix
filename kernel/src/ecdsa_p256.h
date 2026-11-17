/* ecdsa_p256.h — ECDSA signature verification on NIST P-256 with SHA-256 (FIPS 186-4).
 * Used for X.509 chain signatures and TLS CertificateVerify (ecdsa_secp256r1_sha256).
 * Public values only; not constant-time. */
#ifndef HELIX_ECDSA_P256_H
#define HELIX_ECDSA_P256_H

#include <stddef.h>
#include <stdint.h>

/* pub is the uncompressed point 0x04 || X || Y (65 bytes). sig is the DER encoding
 * SEQUENCE { r INTEGER, s INTEGER }. Returns 1 if the signature is valid. Returns 0 for a
 * malformed key (not on the curve), a malformed or out-of-range signature, or a mismatch. */
int ecdsa_p256_verify_sha256(const uint8_t *pub, size_t pub_len,
                             const uint8_t hash[32], const uint8_t *sig, size_t sig_len);

#endif /* HELIX_ECDSA_P256_H */
