/* x25519.h — X25519 Diffie-Hellman (RFC 7748) for the TLS 1.3 key exchange.
 *
 * Field arithmetic uses 16 limbs of 16 bits held in 64-bit integers, which
 * needs no floating point or 128-bit multiply. Slow, but small and easy to check. */
#ifndef HELIX_X25519_H
#define HELIX_X25519_H

#include <stdint.h>

#define X25519_LEN 32u

/* out = scalar * point (both 32 bytes, little-endian). The scalar is clamped
 * as RFC 7748 specifies. */
void x25519(uint8_t out[X25519_LEN], const uint8_t scalar[X25519_LEN],
            const uint8_t point[X25519_LEN]);

/* out = scalar * 9 (the base point), giving the public key for a private key. */
void x25519_base(uint8_t out[X25519_LEN], const uint8_t scalar[X25519_LEN]);

#endif /* HELIX_X25519_H */
