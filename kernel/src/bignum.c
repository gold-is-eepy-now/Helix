/* bignum.c — see bignum.h. Montgomery multiplication in the CIOS form (Koc, Acar and
 * Kaliski, 1996): multiply and reduce one word of the multiplier at a time, so the
 * intermediate never grows past k+2 words and no division is needed. */
#include "bignum.h"
#include "kstring.h"

int bn_from_be(uint32_t *w, size_t k, const uint8_t *be, size_t len) {
    /* Skip leading zero bytes; what is left must fit in k words. */
    while (len > 0 && *be == 0) { be++; len--; }
    if (len > k * 4) return 0;
    memset(w, 0, k * sizeof(uint32_t));
    for (size_t i = 0; i < len; i++) {
        size_t bit_index = len - 1 - i;              /* byte position from the least significant end */
        w[bit_index / 4] |= (uint32_t)be[i] << (8 * (bit_index % 4));
    }
    return 1;
}

void bn_to_be(const uint32_t *w, size_t k, uint8_t *be, size_t len) {
    for (size_t i = 0; i < len; i++) {
        size_t pos = len - 1 - i;                    /* least significant byte is last */
        size_t word = pos / 4;
        be[i] = word < k ? (uint8_t)(w[word] >> (8 * (pos % 4))) : 0;
    }
}

int bn_cmp(const uint32_t *a, const uint32_t *b, size_t k) {
    for (size_t i = k; i-- > 0;) {
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}

void bn_sub(uint32_t *a, const uint32_t *b, size_t k) {
    uint64_t borrow = 0;
    for (size_t i = 0; i < k; i++) {
        uint64_t d = (uint64_t)a[i] - b[i] - borrow;
        a[i] = (uint32_t)d;
        borrow = (d >> 63) & 1u;                     /* wrapped below zero */
    }
}

size_t bn_bits(const uint32_t *w, size_t k) {
    for (size_t i = k; i-- > 0;) {
        if (w[i] == 0) continue;
        size_t bits = 0;
        for (uint32_t v = w[i]; v != 0; v >>= 1) bits++;
        return i * 32 + bits;
    }
    return 0;
}

/* Inverse of an odd word modulo 2^32 by Newton iteration: each step doubles the number
 * of correct low bits, so five steps give 32. */
static uint32_t inv_word(uint32_t n0) {
    uint32_t x = n0;                                 /* correct to 3 bits for odd n0 */
    for (int i = 0; i < 5; i++) x *= 2u - n0 * x;
    return x;
}

/* r = (r + r) mod n, for r < n. */
static void double_mod(uint32_t *r, const uint32_t *n, size_t k) {
    uint32_t carry = 0;
    for (size_t i = 0; i < k; i++) {
        uint32_t next = r[i] >> 31;
        r[i] = (r[i] << 1) | carry;
        carry = next;
    }
    /* Value is now r + carry*2^(32k). Subtract n while it is >= n; at most twice. */
    if (carry || bn_cmp(r, n, k) >= 0) bn_sub(r, n, k);
}

int mont_init(mont_t *m, const uint32_t *n, size_t k) {
    if (k == 0 || k > BN_MAX_WORDS) return 0;
    if ((n[0] & 1u) == 0) return 0;                  /* Montgomery needs an odd modulus */
    if (bn_bits(n, k) == 0) return 0;
    m->k = k;
    memcpy(m->n, n, k * sizeof(uint32_t));
    m->ninv = (uint32_t)(0u - inv_word(n[0]));

    /* R^2 mod n: start from 1 and double 2*32k times. Each doubling is a shift and a
     * conditional subtract, so no division is needed. */
    memset(m->r2, 0, sizeof m->r2);
    m->r2[0] = 1;
    for (size_t i = 0; i < 2 * 32 * k; i++) double_mod(m->r2, m->n, k);
    return 1;
}

void mont_mul(const mont_t *m, uint32_t *r, const uint32_t *a, const uint32_t *b) {
    size_t k = m->k;
    uint32_t t[BN_MAX_WORDS + 2];
    memset(t, 0, (k + 2) * sizeof(uint32_t));

    for (size_t i = 0; i < k; i++) {
        /* t += a * b[i] */
        uint64_t carry = 0;
        for (size_t j = 0; j < k; j++) {
            uint64_t x = (uint64_t)t[j] + (uint64_t)a[j] * b[i] + carry;
            t[j] = (uint32_t)x;
            carry = x >> 32;
        }
        uint64_t x = (uint64_t)t[k] + carry;
        t[k] = (uint32_t)x;
        t[k + 1] = (uint32_t)(x >> 32);

        /* Add m * n so the low word becomes zero, then shift right one word. */
        uint32_t mu = t[0] * m->ninv;
        x = (uint64_t)t[0] + (uint64_t)mu * m->n[0];
        carry = x >> 32;
        for (size_t j = 1; j < k; j++) {
            x = (uint64_t)t[j] + (uint64_t)mu * m->n[j] + carry;
            t[j - 1] = (uint32_t)x;
            carry = x >> 32;
        }
        x = (uint64_t)t[k] + carry;
        t[k - 1] = (uint32_t)x;
        carry = x >> 32;
        t[k] = t[k + 1] + (uint32_t)carry;
        t[k + 1] = 0;
    }

    /* The result is below 2n, so one subtraction brings it into range. */
    if (t[k] != 0 || bn_cmp(t, m->n, k) >= 0) bn_sub(t, m->n, k);
    memcpy(r, t, k * sizeof(uint32_t));
}

void mont_modexp(const mont_t *m, uint32_t *out, const uint32_t *base,
                 const uint8_t *e_be, size_t elen) {
    uint32_t x[BN_MAX_WORDS], acc[BN_MAX_WORDS], one[BN_MAX_WORDS];

    memset(one, 0, sizeof one);
    one[0] = 1;
    mont_mul(m, x, base, m->r2);                     /* base into Montgomery form */
    mont_mul(m, acc, one, m->r2);                    /* 1 into Montgomery form (R mod n) */

    /* Left-to-right square and multiply over every exponent bit. */
    for (size_t i = 0; i < elen; i++) {
        for (int bit = 7; bit >= 0; bit--) {
            mont_mul(m, acc, acc, acc);
            if ((e_be[i] >> bit) & 1u) mont_mul(m, acc, acc, x);
        }
    }

    mont_mul(m, out, acc, one);                      /* back out of Montgomery form */
}
