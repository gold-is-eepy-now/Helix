/* tls13_schedule.c — see tls13_schedule.h. */
#include "tls13_schedule.h"

#include <stddef.h>
#include <stdint.h>

#include "hkdf.h"
#include "kstring.h"

void tls13_expand_label(uint8_t *out, size_t len, const uint8_t secret[SHA256_DIGEST_LEN],
                        const char *label, const uint8_t *context, size_t context_len) {
    /* struct { uint16 length; opaque label<7..255>; opaque context<0..255>; }
     * with the label prefixed by "tls13 ". */
    static const char prefix[] = "tls13 ";
    uint8_t info[2 + 1 + 6 + 32 + 1 + 64];
    size_t label_len = strlen(label);
    size_t n = 0;
    info[n++] = (uint8_t)(len >> 8);
    info[n++] = (uint8_t)len;
    info[n++] = (uint8_t)(6 + label_len);
    memcpy(info + n, prefix, 6);
    n += 6;
    memcpy(info + n, label, label_len);
    n += label_len;
    info[n++] = (uint8_t)context_len;
    memcpy(info + n, context, context_len);
    n += context_len;
    hkdf_expand(out, len, secret, info, n);
}

void tls13_derive_secret(uint8_t out[SHA256_DIGEST_LEN], const uint8_t secret[SHA256_DIGEST_LEN],
                         const char *label, const uint8_t transcript_hash[SHA256_DIGEST_LEN]) {
    tls13_expand_label(out, SHA256_DIGEST_LEN, secret, label, transcript_hash, SHA256_DIGEST_LEN);
}

void tls13_traffic_keys(uint8_t *key, size_t key_len, uint8_t iv[12],
                        const uint8_t traffic_secret[SHA256_DIGEST_LEN]) {
    tls13_expand_label(key, key_len, traffic_secret, "key", NULL, 0);
    tls13_expand_label(iv, 12, traffic_secret, "iv", NULL, 0);
}
