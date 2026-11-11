/* chacha20poly1305.h — the ChaCha20-Poly1305 AEAD (RFC 8439), TLS 1.3's
 * TLS_CHACHA20_POLY1305_SHA256 cipher suite. Integer code only. */
#ifndef HELIX_CHACHA20POLY1305_H
#define HELIX_CHACHA20POLY1305_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AEAD_KEY_LEN   32u
#define AEAD_NONCE_LEN 12u
#define AEAD_TAG_LEN   16u

/* Encrypts `len` bytes in place-compatible fashion: out may equal in. Writes the tag. */
void aead_chacha20poly1305_seal(uint8_t *out, uint8_t tag[AEAD_TAG_LEN],
                                const uint8_t key[AEAD_KEY_LEN],
                                const uint8_t nonce[AEAD_NONCE_LEN],
                                const uint8_t *aad, size_t aad_len,
                                const uint8_t *in, size_t len);

/* Checks the tag before writing any plaintext. Returns false, and leaves out
 * untouched, when the tag does not match. out may equal in. */
bool aead_chacha20poly1305_open(uint8_t *out, const uint8_t tag[AEAD_TAG_LEN],
                                const uint8_t key[AEAD_KEY_LEN],
                                const uint8_t nonce[AEAD_NONCE_LEN],
                                const uint8_t *aad, size_t aad_len,
                                const uint8_t *in, size_t len);

/* Exposed for the RFC 8439 test vectors. */
void chacha20_xor(uint8_t *out, const uint8_t *in, size_t len,
                  const uint8_t key[32], const uint8_t nonce[12], uint32_t counter);

#endif /* HELIX_CHACHA20POLY1305_H */
