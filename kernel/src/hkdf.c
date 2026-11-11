/* hkdf.c — see hkdf.h. */
#include "hkdf.h"

#include <stddef.h>
#include <stdint.h>

#include "kstring.h"

void hkdf_extract(uint8_t prk[SHA256_DIGEST_LEN],
                  const uint8_t *salt, size_t salt_len,
                  const uint8_t *ikm, size_t ikm_len) {
    hmac_sha256(salt, salt_len, ikm, ikm_len, prk);
}

/* T(i) = HMAC(PRK, T(i-1) | info | i), with T(0) empty; the output is T(1)|T(2)|... */
void hkdf_expand(uint8_t *out, size_t out_len,
                 const uint8_t prk[SHA256_DIGEST_LEN],
                 const uint8_t *info, size_t info_len) {
    uint8_t t[SHA256_DIGEST_LEN];
    size_t t_len = 0;
    uint8_t counter = 1;
    size_t done = 0;

    /* msg holds T(i-1), the info and the counter; the largest info callers pass is small. */
    uint8_t msg[SHA256_DIGEST_LEN + 128 + 1];
    while (done < out_len) {
        size_t n = 0;
        memcpy(msg + n, t, t_len);
        n += t_len;
        memcpy(msg + n, info, info_len);
        n += info_len;
        msg[n++] = counter++;
        hmac_sha256(prk, SHA256_DIGEST_LEN, msg, n, t);
        t_len = SHA256_DIGEST_LEN;

        size_t take = out_len - done < SHA256_DIGEST_LEN ? out_len - done : SHA256_DIGEST_LEN;
        memcpy(out + done, t, take);
        done += take;
    }
}
