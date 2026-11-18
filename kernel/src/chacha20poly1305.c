/* chacha20poly1305.c — see chacha20poly1305.h. */
#include "chacha20poly1305.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kstring.h"

static uint32_t rotl32(uint32_t x, unsigned n) {
    return (x << n) | (x >> (32u - n));
}

static uint32_t load32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void store32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

#define QR(a, b, c, d)                       \
    do {                                     \
        a += b; d ^= a; d = rotl32(d, 16);   \
        c += d; b ^= c; b = rotl32(b, 12);   \
        a += b; d ^= a; d = rotl32(d, 8);    \
        c += d; b ^= c; b = rotl32(b, 7);    \
    } while (0)

/* One 64-byte ChaCha20 block: 20 rounds (10 double rounds) over the state, then
 * the state is added to its own input. */
static void chacha20_block(uint8_t out[64], const uint32_t in[16]) {
    uint32_t x[16];
    for (int i = 0; i < 16; i++) x[i] = in[i];
    for (int i = 0; i < 10; i++) {
        QR(x[0], x[4], x[8], x[12]);
        QR(x[1], x[5], x[9], x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]);
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8], x[13]);
        QR(x[3], x[4], x[9], x[14]);
    }
    for (int i = 0; i < 16; i++) store32le(out + 4 * i, x[i] + in[i]);
}

void chacha20_xor(uint8_t *out, const uint8_t *in, size_t len,
                  const uint8_t key[32], const uint8_t nonce[12], uint32_t counter) {
    uint32_t state[16];
    state[0] = 0x61707865u;   /* "expa" */
    state[1] = 0x3320646eu;   /* "nd 3" */
    state[2] = 0x79622d32u;   /* "2-by" */
    state[3] = 0x6b206574u;   /* "te k" */
    for (int i = 0; i < 8; i++) state[4 + i] = load32le(key + 4 * i);
    state[12] = counter;
    for (int i = 0; i < 3; i++) state[13 + i] = load32le(nonce + 4 * i);

    uint8_t block[64];
    size_t done = 0;
    while (done < len) {
        chacha20_block(block, state);
        state[12]++;
        size_t n = len - done < 64 ? len - done : 64;
        for (size_t i = 0; i < n; i++) out[done + i] = in[done + i] ^ block[i];
        done += n;
    }
}

/* Poly1305 (RFC 8439 2.5) with a 130-bit accumulator in five 26-bit limbs, so
 * every product fits a 64-bit integer. */
typedef struct {
    uint32_t r0, r1, r2, r3, r4;
    uint32_t s1, s2, s3, s4;    /* 5 * r, folded in for the 2^130 = 5 wrap */
    uint32_t h0, h1, h2, h3, h4;
    uint32_t pad[4];
    uint8_t buf[16];
    size_t buf_len;
} poly1305_t;

static void poly_init(poly1305_t *st, const uint8_t key[32]) {
    uint32_t t0 = load32le(key + 0), t1 = load32le(key + 4);
    uint32_t t2 = load32le(key + 8), t3 = load32le(key + 12);
    st->r0 = t0 & 0x3ffffff;
    st->r1 = ((t0 >> 26) | (t1 << 6)) & 0x3ffff03;
    st->r2 = ((t1 >> 20) | (t2 << 12)) & 0x3ffc0ff;
    st->r3 = ((t2 >> 14) | (t3 << 18)) & 0x3f03fff;
    st->r4 = (t3 >> 8) & 0x00fffff;
    st->s1 = st->r1 * 5;
    st->s2 = st->r2 * 5;
    st->s3 = st->r3 * 5;
    st->s4 = st->r4 * 5;
    st->h0 = st->h1 = st->h2 = st->h3 = st->h4 = 0;
    for (int i = 0; i < 4; i++) st->pad[i] = load32le(key + 16 + 4 * i);
    st->buf_len = 0;
}

/* Absorb one 16-byte block. hibit is 1<<24 for full blocks (the implicit 0x01
 * byte above the message) and 0 for a final block that already has its 0x01. */
static void poly_block(poly1305_t *st, const uint8_t m[16], uint32_t hibit) {
    uint32_t t0 = load32le(m + 0), t1 = load32le(m + 4);
    uint32_t t2 = load32le(m + 8), t3 = load32le(m + 12);
    st->h0 += t0 & 0x3ffffff;
    st->h1 += ((t0 >> 26) | (t1 << 6)) & 0x3ffffff;
    st->h2 += ((t1 >> 20) | (t2 << 12)) & 0x3ffffff;
    st->h3 += ((t2 >> 14) | (t3 << 18)) & 0x3ffffff;
    st->h4 += (t3 >> 8) | hibit;

    uint64_t d0 = (uint64_t)st->h0 * st->r0 + (uint64_t)st->h1 * st->s4 + (uint64_t)st->h2 * st->s3 +
                  (uint64_t)st->h3 * st->s2 + (uint64_t)st->h4 * st->s1;
    uint64_t d1 = (uint64_t)st->h0 * st->r1 + (uint64_t)st->h1 * st->r0 + (uint64_t)st->h2 * st->s4 +
                  (uint64_t)st->h3 * st->s3 + (uint64_t)st->h4 * st->s2;
    uint64_t d2 = (uint64_t)st->h0 * st->r2 + (uint64_t)st->h1 * st->r1 + (uint64_t)st->h2 * st->r0 +
                  (uint64_t)st->h3 * st->s4 + (uint64_t)st->h4 * st->s3;
    uint64_t d3 = (uint64_t)st->h0 * st->r3 + (uint64_t)st->h1 * st->r2 + (uint64_t)st->h2 * st->r1 +
                  (uint64_t)st->h3 * st->r0 + (uint64_t)st->h4 * st->s4;
    uint64_t d4 = (uint64_t)st->h0 * st->r4 + (uint64_t)st->h1 * st->r3 + (uint64_t)st->h2 * st->r2 +
                  (uint64_t)st->h3 * st->r1 + (uint64_t)st->h4 * st->r0;

    uint32_t c = (uint32_t)(d0 >> 26); st->h0 = (uint32_t)d0 & 0x3ffffff;
    d1 += c; c = (uint32_t)(d1 >> 26); st->h1 = (uint32_t)d1 & 0x3ffffff;
    d2 += c; c = (uint32_t)(d2 >> 26); st->h2 = (uint32_t)d2 & 0x3ffffff;
    d3 += c; c = (uint32_t)(d3 >> 26); st->h3 = (uint32_t)d3 & 0x3ffffff;
    d4 += c; c = (uint32_t)(d4 >> 26); st->h4 = (uint32_t)d4 & 0x3ffffff;
    st->h0 += c * 5; c = st->h0 >> 26; st->h0 &= 0x3ffffff;
    st->h1 += c;
}

static void poly_update(poly1305_t *st, const uint8_t *m, size_t len) {
    while (len > 0) {
        size_t take = 16 - st->buf_len;
        if (take > len) take = len;
        memcpy(st->buf + st->buf_len, m, take);
        st->buf_len += take;
        m += take;
        len -= take;
        if (st->buf_len == 16) {
            poly_block(st, st->buf, 1u << 24);
            st->buf_len = 0;
        }
    }
}

static void poly_finish(poly1305_t *st, uint8_t tag[16]) {
    if (st->buf_len > 0) {
        /* A partial block gets the 0x01 marker and zero padding, with no implicit 0x01. */
        uint8_t last[16];
        memset(last, 0, sizeof last);
        memcpy(last, st->buf, st->buf_len);
        last[st->buf_len] = 1;
        poly_block(st, last, 0);
    }

    uint32_t h0 = st->h0, h1 = st->h1, h2 = st->h2, h3 = st->h3, h4 = st->h4;
    uint32_t c = h1 >> 26; h1 &= 0x3ffffff;
    h2 += c; c = h2 >> 26; h2 &= 0x3ffffff;
    h3 += c; c = h3 >> 26; h3 &= 0x3ffffff;
    h4 += c; c = h4 >> 26; h4 &= 0x3ffffff;
    h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff;
    h1 += c;

    /* Compute h - p; keep it only when h >= p, selected without a branch. */
    uint32_t g0 = h0 + 5; c = g0 >> 26; g0 &= 0x3ffffff;
    uint32_t g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffff;
    uint32_t g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffff;
    uint32_t g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffff;
    uint32_t g4 = h4 + c - (1u << 26);
    uint32_t mask = (g4 >> 31) - 1;   /* all ones when h >= p */
    g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0;
    h1 = (h1 & mask) | g1;
    h2 = (h2 & mask) | g2;
    h3 = (h3 & mask) | g3;
    h4 = (h4 & mask) | g4;

    /* Repack into four 32-bit words and add the pad s, mod 2^128. */
    uint32_t w0 = h0 | (h1 << 26);
    uint32_t w1 = (h1 >> 6) | (h2 << 20);
    uint32_t w2 = (h2 >> 12) | (h3 << 14);
    uint32_t w3 = (h3 >> 18) | (h4 << 8);
    uint64_t f = (uint64_t)w0 + st->pad[0];
    store32le(tag + 0, (uint32_t)f);
    f = (uint64_t)w1 + st->pad[1] + (f >> 32);
    store32le(tag + 4, (uint32_t)f);
    f = (uint64_t)w2 + st->pad[2] + (f >> 32);
    store32le(tag + 8, (uint32_t)f);
    f = (uint64_t)w3 + st->pad[3] + (f >> 32);
    store32le(tag + 12, (uint32_t)f);
}

/* The one-time Poly1305 key is the first 32 bytes of the ChaCha20 block at counter 0. */
static void compute_tag(uint8_t tag[16], const uint8_t key[32], const uint8_t nonce[12],
                        const uint8_t *aad, size_t aad_len,
                        const uint8_t *ct, size_t ct_len) {
    uint8_t zero[64];
    memset(zero, 0, sizeof zero);
    uint8_t block0[64];
    chacha20_xor(block0, zero, sizeof block0, key, nonce, 0);

    poly1305_t st;
    poly_init(&st, block0);
    static const uint8_t pad_bytes[16] = { 0 };
    poly_update(&st, aad, aad_len);
    poly_update(&st, pad_bytes, (16 - aad_len % 16) % 16);
    poly_update(&st, ct, ct_len);
    poly_update(&st, pad_bytes, (16 - ct_len % 16) % 16);
    uint8_t lens[16];
    for (int i = 0; i < 8; i++) lens[i] = (uint8_t)((uint64_t)aad_len >> (8 * i));
    for (int i = 0; i < 8; i++) lens[8 + i] = (uint8_t)((uint64_t)ct_len >> (8 * i));
    poly_update(&st, lens, sizeof lens);
    poly_finish(&st, tag);
}

void aead_chacha20poly1305_seal(uint8_t *out, uint8_t tag[AEAD_TAG_LEN],
                                const uint8_t key[AEAD_KEY_LEN],
                                const uint8_t nonce[AEAD_NONCE_LEN],
                                const uint8_t *aad, size_t aad_len,
                                const uint8_t *in, size_t len) {
    chacha20_xor(out, in, len, key, nonce, 1);
    compute_tag(tag, key, nonce, aad, aad_len, out, len);
}

bool aead_chacha20poly1305_open(uint8_t *out, const uint8_t tag[AEAD_TAG_LEN],
                                const uint8_t key[AEAD_KEY_LEN],
                                const uint8_t nonce[AEAD_NONCE_LEN],
                                const uint8_t *aad, size_t aad_len,
                                const uint8_t *in, size_t len) {
    uint8_t expect[AEAD_TAG_LEN];
    compute_tag(expect, key, nonce, aad, aad_len, in, len);
    uint8_t diff = 0;
    for (size_t i = 0; i < AEAD_TAG_LEN; i++) diff |= (uint8_t)(expect[i] ^ tag[i]);
    if (diff != 0) return false;
    chacha20_xor(out, in, len, key, nonce, 1);
    return true;
}
