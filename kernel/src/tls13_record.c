/* tls13_record.c — see tls13_record.h. */
#include "tls13_record.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "chacha20poly1305.h"
#include "kstring.h"

void tls13_nonce(uint8_t nonce[12], const uint8_t iv[12], uint64_t seq) {
    memcpy(nonce, iv, 12);
    for (int i = 0; i < 8; i++) {
        nonce[4 + i] ^= (uint8_t)(seq >> (56 - 8 * i));
    }
}

/* The record header is also the AEAD additional data. */
static void header(uint8_t hdr[5], uint16_t len) {
    hdr[0] = TLS_CT_APPDATA;   /* outer type is always application_data */
    hdr[1] = 0x03;
    hdr[2] = 0x03;             /* legacy version 0x0303 */
    hdr[3] = (uint8_t)(len >> 8);
    hdr[4] = (uint8_t)len;
}

size_t tls13_seal(uint8_t *out, tls13_cipher_t *c, uint8_t content_type,
                  const uint8_t *plain, size_t plain_len) {
    size_t inner_len = plain_len + 1;
    uint16_t body_len = (uint16_t)(inner_len + TLS_TAG_LEN);
    uint8_t nonce[12];

    header(out, body_len);
    /* Build the inner plaintext (data || content type) in the output buffer. */
    memcpy(out + TLS_RECORD_HDR_LEN, plain, plain_len);
    out[TLS_RECORD_HDR_LEN + plain_len] = content_type;

    tls13_nonce(nonce, c->iv, c->seq);
    /* Encrypt in place: out and in point at the same bytes. */
    aead_chacha20poly1305_seal(out + TLS_RECORD_HDR_LEN, out + TLS_RECORD_HDR_LEN + inner_len,
                               c->key, nonce, out, TLS_RECORD_HDR_LEN,
                               out + TLS_RECORD_HDR_LEN, inner_len);
    c->seq++;
    return TLS_RECORD_HDR_LEN + body_len;
}

bool tls13_open(uint8_t *plain, size_t *plain_len, uint8_t *content_type,
                tls13_cipher_t *c, const uint8_t *rec, size_t rec_len) {
    if (rec_len < TLS_RECORD_HDR_LEN + TLS_TAG_LEN + 1) return false;
    if (rec[0] != TLS_CT_APPDATA || rec[1] != 0x03 || rec[2] != 0x03) return false;
    size_t body_len = ((size_t)rec[3] << 8) | rec[4];
    if (body_len + TLS_RECORD_HDR_LEN != rec_len) return false;
    if (body_len < TLS_TAG_LEN + 1 || body_len > TLS_MAX_PLAIN + 1 + TLS_TAG_LEN) return false;

    size_t inner_len = body_len - TLS_TAG_LEN;
    const uint8_t *tag = rec + TLS_RECORD_HDR_LEN + inner_len;
    uint8_t nonce[12];
    tls13_nonce(nonce, c->iv, c->seq);

    /* Decrypt to a scratch area inside plain, so a failed tag leaves caller data alone. */
    uint8_t *inner = plain;
    if (!aead_chacha20poly1305_open(inner, tag, c->key, nonce, rec, TLS_RECORD_HDR_LEN,
                                    rec + TLS_RECORD_HDR_LEN, inner_len)) {
        return false;
    }

    /* Strip zero padding; the last non-zero byte is the real content type. */
    size_t end = inner_len;
    while (end > 0 && inner[end - 1] == 0) end--;
    if (end == 0) return false;
    *content_type = inner[end - 1];
    *plain_len = end - 1;
    c->seq++;
    return true;
}
