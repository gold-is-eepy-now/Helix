/* ecdsa_p256.c — ECDSA verification on P-256. Field and scalar arithmetic reuse the
 * Montgomery context from bignum.c (both moduli are odd primes). Points use Jacobian
 * coordinates, so a point needs no inversion until the final affine x is read out; that
 * inversion is Fermat's little theorem, x^(p-2). Points are kept in Montgomery form. */
#include "ecdsa_p256.h"
#include "bignum.h"
#include "kstring.h"

#define W 8u   /* 256-bit values as 32-bit words */

/* Curve parameters, big-endian (SEC 2 / FIPS 186-4). */
static const uint8_t P_BE[32] = {
    0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};
static const uint8_t N_BE[32] = {
    0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xbc, 0xe6, 0xfa, 0xad, 0xa7, 0x17, 0x9e, 0x84, 0xf3, 0xb9, 0xca, 0xc2, 0xfc, 0x63, 0x25, 0x51,
};
static const uint8_t B_BE[32] = {
    0x5a, 0xc6, 0x35, 0xd8, 0xaa, 0x3a, 0x93, 0xe7, 0xb3, 0xeb, 0xbd, 0x55, 0x76, 0x98, 0x86, 0xbc,
    0x65, 0x1d, 0x06, 0xb0, 0xcc, 0x53, 0xb0, 0xf6, 0x3b, 0xce, 0x3c, 0x3e, 0x27, 0xd2, 0x60, 0x4b,
};
static const uint8_t GX_BE[32] = {
    0x6b, 0x17, 0xd1, 0xf2, 0xe1, 0x2c, 0x42, 0x47, 0xf8, 0xbc, 0xe6, 0xe5, 0x63, 0xa4, 0x40, 0xf2,
    0x77, 0x03, 0x7d, 0x81, 0x2d, 0xeb, 0x33, 0xa0, 0xf4, 0xa1, 0x39, 0x45, 0xd8, 0x98, 0xc2, 0x96,
};
static const uint8_t GY_BE[32] = {
    0x4f, 0xe3, 0x42, 0xe2, 0xfe, 0x1a, 0x7f, 0x9b, 0x8e, 0xe7, 0xeb, 0x4a, 0x7c, 0x0f, 0x9e, 0x16,
    0x2b, 0xce, 0x33, 0x57, 0x6b, 0x31, 0x5e, 0xce, 0xcb, 0xb6, 0x40, 0x68, 0x37, 0xbf, 0x51, 0xf5,
};

typedef struct { uint32_t x[W], y[W], z[W]; } jpoint_t;   /* Jacobian; z == 0 is infinity */

typedef struct {
    mont_t fp;            /* field modulo p */
    mont_t fn;            /* scalars modulo n */
    uint32_t b[W];        /* curve b, Montgomery form */
    uint32_t one[W];      /* 1, Montgomery form */
    uint32_t gx[W], gy[W];/* base point, Montgomery form */
} ctx_t;

/* ---- field helpers (values in Montgomery form, reduced below p) ---- */

static void fe_add(const mont_t *m, uint32_t *r, const uint32_t *a, const uint32_t *b) {
    uint32_t t[W];
    uint64_t carry = 0;
    for (size_t i = 0; i < W; i++) {
        uint64_t s = (uint64_t)a[i] + b[i] + carry;
        t[i] = (uint32_t)s;
        carry = s >> 32;
    }
    if (carry || bn_cmp(t, m->n, W) >= 0) bn_sub(t, m->n, W);
    memcpy(r, t, sizeof t);
}

static void fe_sub(const mont_t *m, uint32_t *r, const uint32_t *a, const uint32_t *b) {
    uint32_t t[W];
    uint64_t borrow = 0;
    for (size_t i = 0; i < W; i++) {
        uint64_t d = (uint64_t)a[i] - b[i] - borrow;
        t[i] = (uint32_t)d;
        borrow = (d >> 63) & 1u;
    }
    if (borrow) {                                    /* went below zero: add p back */
        uint64_t carry = 0;
        for (size_t i = 0; i < W; i++) {
            uint64_t s = (uint64_t)t[i] + m->n[i] + carry;
            t[i] = (uint32_t)s;
            carry = s >> 32;
        }
    }
    memcpy(r, t, sizeof t);
}

static void fe_mul(const mont_t *m, uint32_t *r, const uint32_t *a, const uint32_t *b) {
    mont_mul(m, r, a, b);
}

static int fe_is_zero(const uint32_t *a) {
    uint32_t acc = 0;
    for (size_t i = 0; i < W; i++) acc |= a[i];
    return acc == 0;
}

/* Inverse of a Montgomery-form value, returned in Montgomery form. Uses x^(p-2). */
static void fe_inv(const ctx_t *c, uint32_t *r, const uint32_t *a) {
    uint32_t one_plain[W] = { 1 }, plain[W], inv_plain[W];
    mont_mul(&c->fp, plain, a, one_plain);           /* leave Montgomery form */
    uint8_t exp[32];
    memcpy(exp, P_BE, 32);
    exp[31] -= 2;                                    /* p - 2 (low byte of p is 0xff) */
    mont_modexp(&c->fp, inv_plain, plain, exp, 32);  /* plain x^(p-2) = plain inverse */
    mont_mul(&c->fp, r, inv_plain, c->fp.r2);        /* back to Montgomery form */
}

/* ---- Jacobian point arithmetic for y^2 = x^3 - 3x + b ---- */

static int pt_is_inf(const jpoint_t *p) {
    return fe_is_zero(p->z);
}

static void pt_double(const ctx_t *c, jpoint_t *r, const jpoint_t *p) {
    const mont_t *m = &c->fp;
    if (pt_is_inf(p) || fe_is_zero(p->y)) {
        memset(r, 0, sizeof *r);                     /* 2*P is infinity when y == 0 */
        return;
    }
    uint32_t delta[W], gamma[W], beta[W], alpha[W], t1[W], t2[W], x3[W], y3[W], z3[W], tmp[W];

    fe_mul(m, delta, p->z, p->z);                    /* delta = Z^2 */
    fe_mul(m, gamma, p->y, p->y);                    /* gamma = Y^2 */
    fe_mul(m, beta, p->x, gamma);                    /* beta = X*gamma */

    fe_sub(m, t1, p->x, delta);                      /* alpha = 3*(X-delta)*(X+delta) */
    fe_add(m, t2, p->x, delta);
    fe_mul(m, alpha, t1, t2);
    fe_add(m, tmp, alpha, alpha);
    fe_add(m, alpha, tmp, alpha);

    uint32_t beta4[W], beta8[W];                     /* 4*beta and 8*beta */
    fe_add(m, beta4, beta, beta);
    fe_add(m, beta4, beta4, beta4);
    fe_add(m, beta8, beta4, beta4);

    fe_mul(m, x3, alpha, alpha);                     /* X3 = alpha^2 - 8*beta */
    fe_sub(m, x3, x3, beta8);

    fe_add(m, tmp, p->y, p->z);                      /* Z3 = (Y+Z)^2 - gamma - delta */
    fe_mul(m, z3, tmp, tmp);
    fe_sub(m, z3, z3, gamma);
    fe_sub(m, z3, z3, delta);

    uint32_t gamma2[W], gamma8[W];                   /* Y3 = alpha*(4*beta - X3) - 8*gamma^2 */
    fe_mul(m, gamma2, gamma, gamma);
    fe_add(m, gamma8, gamma2, gamma2);
    fe_add(m, gamma8, gamma8, gamma8);
    fe_add(m, gamma8, gamma8, gamma8);
    fe_sub(m, tmp, beta4, x3);
    fe_mul(m, y3, alpha, tmp);
    fe_sub(m, y3, y3, gamma8);

    memcpy(r->x, x3, sizeof x3);
    memcpy(r->y, y3, sizeof y3);
    memcpy(r->z, z3, sizeof z3);
}

static void pt_add(const ctx_t *c, jpoint_t *r, const jpoint_t *p, const jpoint_t *q) {
    const mont_t *m = &c->fp;
    if (pt_is_inf(p)) { *r = *q; return; }
    if (pt_is_inf(q)) { *r = *p; return; }

    uint32_t z1z1[W], z2z2[W], u1[W], u2[W], s1[W], s2[W], h[W], rr[W], tmp[W], tmp2[W];
    fe_mul(m, z1z1, p->z, p->z);
    fe_mul(m, z2z2, q->z, q->z);
    fe_mul(m, u1, p->x, z2z2);
    fe_mul(m, u2, q->x, z1z1);
    fe_mul(m, tmp, q->z, z2z2);
    fe_mul(m, s1, p->y, tmp);                        /* S1 = Y1*Z2^3 */
    fe_mul(m, tmp, p->z, z1z1);
    fe_mul(m, s2, q->y, tmp);                        /* S2 = Y2*Z1^3 */
    fe_sub(m, h, u2, u1);
    fe_sub(m, rr, s2, s1);
    fe_add(m, rr, rr, rr);

    if (fe_is_zero(h)) {
        if (fe_is_zero(rr)) { pt_double(c, r, p); return; }   /* same point */
        memset(r, 0, sizeof *r);                               /* P and -P: infinity */
        return;
    }

    uint32_t i2[W], j[W], v[W], x3[W], y3[W], z3[W];
    fe_add(m, tmp, h, h);
    fe_mul(m, i2, tmp, tmp);                         /* I = (2H)^2 */
    fe_mul(m, j, h, i2);                             /* J = H*I */
    fe_mul(m, v, u1, i2);                            /* V = U1*I */

    fe_mul(m, x3, rr, rr);                           /* X3 = R^2 - J - 2V */
    fe_sub(m, x3, x3, j);
    fe_sub(m, x3, x3, v);
    fe_sub(m, x3, x3, v);

    fe_sub(m, tmp, v, x3);                           /* Y3 = R*(V - X3) - 2*S1*J */
    fe_mul(m, y3, rr, tmp);
    fe_mul(m, tmp, s1, j);
    fe_add(m, tmp, tmp, tmp);
    fe_sub(m, y3, y3, tmp);

    fe_add(m, tmp, p->z, q->z);                      /* Z3 = ((Z1+Z2)^2 - Z1Z1 - Z2Z2)*H */
    fe_mul(m, z3, tmp, tmp);
    fe_sub(m, z3, z3, z1z1);
    fe_sub(m, z3, z3, z2z2);
    fe_mul(m, tmp2, z3, h);
    memcpy(z3, tmp2, sizeof z3);

    memcpy(r->x, x3, sizeof x3);
    memcpy(r->y, y3, sizeof y3);
    memcpy(r->z, z3, sizeof z3);
}

/* out = k*P for a big-endian scalar k below n. Plain double-and-add over every bit. */
static void pt_mul(const ctx_t *c, jpoint_t *out, const uint8_t k[32], const jpoint_t *pt) {
    jpoint_t acc;
    memset(&acc, 0, sizeof acc);
    for (size_t i = 0; i < 32; i++) {
        for (int bit = 7; bit >= 0; bit--) {
            pt_double(c, &acc, &acc);
            if ((k[i] >> bit) & 1u) pt_add(c, &acc, &acc, pt);
        }
    }
    *out = acc;
}

/* Affine x of a Jacobian point, as a plain value below p. Returns 0 at infinity. */
static int pt_affine_x(const ctx_t *c, uint32_t x_plain[W], const jpoint_t *p) {
    if (pt_is_inf(p)) return 0;
    uint32_t zinv[W], zinv2[W], xa[W], one_plain[W] = { 1 };
    fe_inv(c, zinv, p->z);
    fe_mul(&c->fp, zinv2, zinv, zinv);
    fe_mul(&c->fp, xa, p->x, zinv2);
    mont_mul(&c->fp, x_plain, xa, one_plain);        /* leave Montgomery form */
    return 1;
}

/* Is (x, y) (Montgomery form) on y^2 = x^3 - 3x + b? */
static int on_curve(const ctx_t *c, const uint32_t x[W], const uint32_t y[W]) {
    const mont_t *m = &c->fp;
    uint32_t lhs[W], rhs[W], x2[W], x3[W], t[W];
    fe_mul(m, lhs, y, y);
    fe_mul(m, x2, x, x);
    fe_mul(m, x3, x2, x);
    fe_add(m, t, x, x);
    fe_add(m, t, t, x);                              /* 3x */
    fe_sub(m, rhs, x3, t);
    fe_add(m, rhs, rhs, c->b);
    return bn_cmp(lhs, rhs, W) == 0;
}

/* ---- DER signature parsing: SEQUENCE { INTEGER r, INTEGER s } ---- */

static int der_read_len(const uint8_t **p, const uint8_t *end, size_t *len) {
    if (*p >= end) return 0;
    uint8_t first = *(*p)++;
    if (first < 0x80) { *len = first; return 1; }
    size_t n = first & 0x7f;
    if (n == 0 || n > 2 || (size_t)(end - *p) < n) return 0;   /* only short and 1-2 byte forms */
    size_t v = 0;
    for (size_t i = 0; i < n; i++) v = (v << 8) | *(*p)++;
    *len = v;
    return 1;
}

/* Reads one INTEGER into a 32-byte big-endian buffer (left-padded). The value must be
 * positive and fit in 32 bytes once leading zero bytes are dropped. */
static int der_read_int(const uint8_t **p, const uint8_t *end, uint8_t out[32]) {
    if (*p >= end || *(*p)++ != 0x02) return 0;
    size_t len;
    if (!der_read_len(p, end, &len) || len == 0 || (size_t)(end - *p) < len) return 0;
    const uint8_t *v = *p;
    *p += len;
    if (v[0] & 0x80) return 0;                       /* negative: never valid here */
    while (len > 1 && *v == 0) { v++; len--; }
    if (len > 32) return 0;
    memset(out, 0, 32);
    memcpy(out + (32 - len), v, len);
    return 1;
}

static int parse_sig(const uint8_t *sig, size_t sig_len, uint8_t r[32], uint8_t s[32]) {
    const uint8_t *p = sig, *end = sig + sig_len;
    if (p >= end || *p++ != 0x30) return 0;
    size_t seq_len;
    if (!der_read_len(&p, end, &seq_len) || (size_t)(end - p) != seq_len) return 0;
    if (!der_read_int(&p, end, r)) return 0;
    if (!der_read_int(&p, end, s)) return 0;
    return p == end;                                 /* nothing may follow s */
}

/* ---- public entry point ---- */

int ecdsa_p256_verify_sha256(const uint8_t *pub, size_t pub_len,
                             const uint8_t hash[32], const uint8_t *sig, size_t sig_len) {
    if (pub_len != 65 || pub[0] != 0x04) return 0;

    ctx_t c;
    uint32_t p_w[W], n_w[W];
    if (!bn_from_be(p_w, W, P_BE, 32) || !bn_from_be(n_w, W, N_BE, 32)) return 0;
    if (!mont_init(&c.fp, p_w, W) || !mont_init(&c.fn, n_w, W)) return 0;

    /* Curve constants into Montgomery form. */
    uint32_t plain[W];
    bn_from_be(plain, W, B_BE, 32);  mont_mul(&c.fp, c.b, plain, c.fp.r2);
    bn_from_be(plain, W, GX_BE, 32); mont_mul(&c.fp, c.gx, plain, c.fp.r2);
    bn_from_be(plain, W, GY_BE, 32); mont_mul(&c.fp, c.gy, plain, c.fp.r2);
    memset(plain, 0, sizeof plain);
    plain[0] = 1;
    mont_mul(&c.fp, c.one, plain, c.fp.r2);

    /* Public key: coordinates below p and on the curve. */
    uint32_t qx_p[W], qy_p[W], qx[W], qy[W];
    if (!bn_from_be(qx_p, W, pub + 1, 32) || !bn_from_be(qy_p, W, pub + 33, 32)) return 0;
    if (bn_cmp(qx_p, p_w, W) >= 0 || bn_cmp(qy_p, p_w, W) >= 0) return 0;
    mont_mul(&c.fp, qx, qx_p, c.fp.r2);
    mont_mul(&c.fp, qy, qy_p, c.fp.r2);
    if (!on_curve(&c, qx, qy)) return 0;

    /* Signature: r and s in [1, n-1]. */
    uint8_t r_be[32], s_be[32];
    if (!parse_sig(sig, sig_len, r_be, s_be)) return 0;
    uint32_t r_w[W], s_w[W], e_w[W];
    bn_from_be(r_w, W, r_be, 32);
    bn_from_be(s_w, W, s_be, 32);
    if (fe_is_zero(r_w) || fe_is_zero(s_w)) return 0;
    if (bn_cmp(r_w, n_w, W) >= 0 || bn_cmp(s_w, n_w, W) >= 0) return 0;

    /* e = hash truncated to the bit length of n (256 bits here), reduced below n. */
    bn_from_be(e_w, W, hash, 32);
    if (bn_cmp(e_w, n_w, W) >= 0) bn_sub(e_w, n_w, W);

    /* w = s^-1 mod n; u1 = e*w, u2 = r*w (mod n). */
    uint32_t w_plain[W];
    uint8_t n_minus_2[32];
    memcpy(n_minus_2, N_BE, 32);
    n_minus_2[31] -= 2;
    mont_modexp(&c.fn, w_plain, s_w, n_minus_2, 32); /* plain s^(n-2) = s^-1 mod n */
    /* A plain product of two plain values: x*R (via r2) then times y, the R cancels. */
    uint32_t e_m[W], r_m[W], u1[W], u2[W];
    mont_mul(&c.fn, e_m, e_w, c.fn.r2);              /* e*R */
    mont_mul(&c.fn, u1, e_m, w_plain);               /* e*w, plain */
    mont_mul(&c.fn, r_m, r_w, c.fn.r2);              /* r*R */
    mont_mul(&c.fn, u2, r_m, w_plain);               /* r*w, plain */

    /* P = u1*G + u2*Q, then check x mod n == r. */
    jpoint_t g, q, pa, pb, sum;
    memcpy(g.x, c.gx, sizeof g.x);
    memcpy(g.y, c.gy, sizeof g.y);
    memcpy(g.z, c.one, sizeof g.z);
    memcpy(q.x, qx, sizeof q.x);
    memcpy(q.y, qy, sizeof q.y);
    memcpy(q.z, c.one, sizeof q.z);

    uint8_t u1_be[32], u2_be[32];
    bn_to_be(u1, W, u1_be, 32);                      /* u1 and u2 are already plain */
    bn_to_be(u2, W, u2_be, 32);

    pt_mul(&c, &pa, u1_be, &g);
    pt_mul(&c, &pb, u2_be, &q);
    pt_add(&c, &sum, &pa, &pb);

    uint32_t x_plain[W];
    if (!pt_affine_x(&c, x_plain, &sum)) return 0;
    if (bn_cmp(x_plain, n_w, W) >= 0) bn_sub(x_plain, n_w, W);
    return bn_cmp(x_plain, r_w, W) == 0;
}
