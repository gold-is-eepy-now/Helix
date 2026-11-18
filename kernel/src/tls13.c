/* tls13.c — see tls13.h. */
#include "tls13.h"

#include <stddef.h>
#include <stdint.h>

#include "hkdf.h"
#include "kstring.h"

/* HkdfLabel = uint16 length | opaque label<7..255> ("tls13 " + label) | opaque context<0..255> */
void tls13_expand_label(uint8_t *out, size_t out_len,
                        const uint8_t secret[SHA256_DIGEST_LEN],
                        const char *label,
                        const uint8_t *context, size_t context_len) {
    static const char prefix[] = "tls13 ";
    size_t label_len = strlen(label);
    uint8_t info[2 + 1 + sizeof prefix - 1 + 32 + 1 + 32];
    size_t n = 0;
    info[n++] = (uint8_t)(out_len >> 8);
    info[n++] = (uint8_t)out_len;
    info[n++] = (uint8_t)(sizeof prefix - 1 + label_len);
    memcpy(info + n, prefix, sizeof prefix - 1);
    n += sizeof prefix - 1;
    memcpy(info + n, label, label_len);
    n += label_len;
    info[n++] = (uint8_t)context_len;
    memcpy(info + n, context, context_len);
    n += context_len;
    hkdf_expand(out, out_len, secret, info, n);
}

void tls13_derive_secret(uint8_t out[SHA256_DIGEST_LEN],
                         const uint8_t secret[SHA256_DIGEST_LEN],
                         const char *label,
                         const uint8_t transcript_hash[SHA256_DIGEST_LEN]) {
    tls13_expand_label(out, SHA256_DIGEST_LEN, secret, label, transcript_hash, SHA256_DIGEST_LEN);
}

void tls13_traffic_key_iv(uint8_t key[16], uint8_t iv[12],
                          const uint8_t traffic_secret[SHA256_DIGEST_LEN]) {
    tls13_expand_label(key, 16, traffic_secret, "key", NULL, 0);
    tls13_expand_label(iv, 12, traffic_secret, "iv", NULL, 0);
}

void tls13_early_secret(uint8_t out[SHA256_DIGEST_LEN]) {
    static const uint8_t zeros[SHA256_DIGEST_LEN];   /* no PSK: 32 zero bytes */
    hkdf_extract(out, zeros, sizeof zeros, zeros, sizeof zeros);
}

/* Derived = Derive-Secret(early, "derived", ""): the empty-message hash is SHA-256("). */
static void derived_from(uint8_t out[SHA256_DIGEST_LEN], const uint8_t stage[SHA256_DIGEST_LEN]) {
    uint8_t empty_hash[SHA256_DIGEST_LEN];
    sha256("", 0, empty_hash);
    tls13_derive_secret(out, stage, "derived", empty_hash);
}

void tls13_handshake_secret(uint8_t out[SHA256_DIGEST_LEN],
                            const uint8_t early[SHA256_DIGEST_LEN],
                            const uint8_t ecdhe[32]) {
    uint8_t derived[SHA256_DIGEST_LEN];
    derived_from(derived, early);
    hkdf_extract(out, derived, sizeof derived, ecdhe, 32);
}

void tls13_master_secret(uint8_t out[SHA256_DIGEST_LEN],
                         const uint8_t handshake[SHA256_DIGEST_LEN]) {
    static const uint8_t zeros[SHA256_DIGEST_LEN];
    uint8_t derived[SHA256_DIGEST_LEN];
    derived_from(derived, handshake);
    hkdf_extract(out, derived, sizeof derived, zeros, sizeof zeros);
}
