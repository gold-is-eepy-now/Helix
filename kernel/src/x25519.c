/* x25519.c — see x25519.h. The structure follows the Montgomery ladder of RFC 7748. */
#include "x25519.h"

#include <stddef.h>
#include <stdint.h>

#include "kstring.h"

typedef int64_t gf[16];

static const gf gf_121665 = { 0xDB41, 1 };

/* Propagate carries so each limb is back under 2^16 (the top one wraps with weight 38). */
static void car25519(gf o) {
    for (int i = 0; i < 16; i++) {
        o[i] += (int64_t)1 << 16;
        int64_t c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c << 16;
    }
}

/* Swap p and q when b == 1, without a branch on the secret bit. */
static void sel25519(gf p, gf q, int64_t b) {
    int64_t c = ~(b - 1);
    for (int i = 0; i < 16; i++) {
        int64_t t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

/* Reduce fully mod p = 2^255 - 19 and write 32 little-endian bytes. */
static void pack25519(uint8_t o[32], const gf n) {
    gf m, t;
    for (int i = 0; i < 16; i++) t[i] = n[i];
    car25519(t);
    car25519(t);
    car25519(t);
    for (int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int64_t b = (m[15] >> 16) & 1;
        m[14] &= 0xffff;
        sel25519(t, m, 1 - b);
    }
    for (int i = 0; i < 16; i++) {
        o[2 * i] = (uint8_t)(t[i] & 0xff);
        o[2 * i + 1] = (uint8_t)(t[i] >> 8);
    }
}

static void unpack25519(gf o, const uint8_t n[32]) {
    for (int i = 0; i < 16; i++) o[i] = n[2 * i] + ((int64_t)n[2 * i + 1] << 8);
    o[15] &= 0x7fff;
}

static void gf_add(gf o, const gf a, const gf b) {
    for (int i = 0; i < 16; i++) o[i] = a[i] + b[i];
}

static void gf_sub(gf o, const gf a, const gf b) {
    for (int i = 0; i < 16; i++) o[i] = a[i] - b[i];
}

static void gf_mul(gf o, const gf a, const gf b) {
    int64_t t[31];
    memset(t, 0, sizeof t);
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 16; j++) t[i + j] += a[i] * b[j];
    /* 2^256 = 38 mod p, so the high half folds back with weight 38. */
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; i++) o[i] = t[i];
    car25519(o);
    car25519(o);
}

static void gf_sq(gf o, const gf a) {
    gf_mul(o, a, a);
}

/* o = i^(p-2) = 1/i mod p, by the fixed exponent p - 2. */
static void gf_inv(gf o, const gf i) {
    gf c;
    for (int a = 0; a < 16; a++) c[a] = i[a];
    for (int a = 253; a >= 0; a--) {
        gf_sq(c, c);
        if (a != 2 && a != 4) gf_mul(c, c, i);
    }
    for (int a = 0; a < 16; a++) o[a] = c[a];
}

void x25519(uint8_t out[X25519_LEN], const uint8_t scalar[X25519_LEN],
            const uint8_t point[X25519_LEN]) {
    uint8_t z[32];
    gf x, a, b, c, d, e, f;

    for (int i = 0; i < 31; i++) z[i] = scalar[i];
    z[31] = (uint8_t)((scalar[31] & 127) | 64);   /* clamp: top bit clear, bit 254 set */
    z[0] &= 248;                                  /* clamp: low 3 bits clear */

    unpack25519(x, point);
    for (int i = 0; i < 16; i++) {
        b[i] = x[i];
        d[i] = a[i] = c[i] = 0;
    }
    a[0] = d[0] = 1;

    for (int i = 254; i >= 0; --i) {
        int64_t r = (z[i >> 3] >> (i & 7)) & 1;
        sel25519(a, b, r);
        sel25519(c, d, r);
        gf_add(e, a, c);
        gf_sub(a, a, c);
        gf_add(c, b, d);
        gf_sub(b, b, d);
        gf_sq(d, e);
        gf_sq(f, a);
        gf_mul(a, c, a);
        gf_mul(c, b, e);
        gf_add(e, a, c);
        gf_sub(a, a, c);
        gf_sq(b, a);
        gf_sub(c, d, f);
        gf_mul(a, c, gf_121665);
        gf_add(a, a, d);
        gf_mul(c, c, a);
        gf_mul(a, d, f);
        gf_mul(d, b, x);
        gf_sq(b, e);
        sel25519(a, b, r);
        sel25519(c, d, r);
    }
    /* The result is x2 / z2, so one inversion at the end. */
    gf_inv(c, c);
    gf_mul(a, a, c);
    pack25519(out, a);
}

void x25519_base(uint8_t out[X25519_LEN], const uint8_t scalar[X25519_LEN]) {
    uint8_t nine[32];
    memset(nine, 0, sizeof nine);
    nine[0] = 9;
    x25519(out, scalar, nine);
}
