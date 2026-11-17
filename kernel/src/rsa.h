/* rsa.h — RSA signature verification (RFC 8017) for X.509 chains and TLS CertificateVerify.
 * Public-key operations only. The modulus and exponent are big-endian with no leading
 * zero bytes, as x509.c reports them. Only SHA-256 is wired up: other digests are the
 * caller's job to refuse. */
#ifndef HELIX_RSA_H
#define HELIX_RSA_H

#include <stddef.h>
#include <stdint.h>

/* RSASSA-PKCS1-v1_5 with SHA-256 (x509 sha256WithRSAEncryption). Returns 1 if valid. */
int rsa_verify_pkcs1_sha256(const uint8_t *n, size_t nlen, const uint8_t *e, size_t elen,
                            const uint8_t hash[32], const uint8_t *sig, size_t siglen);

/* RSASSA-PSS with SHA-256, MGF1-SHA-256 and a 32-byte salt (TLS rsa_pss_rsae_sha256).
 * Returns 1 if valid. */
int rsa_verify_pss_sha256(const uint8_t *n, size_t nlen, const uint8_t *e, size_t elen,
                          const uint8_t hash[32], const uint8_t *sig, size_t siglen);

#endif /* HELIX_RSA_H */
