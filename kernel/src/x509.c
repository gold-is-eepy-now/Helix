/* x509.c — see x509.h. */
#include "x509.h"

#include <stddef.h>
#include <stdint.h>

#include "kstring.h"

/* ---- DER reading -------------------------------------------------------- */

/* A cursor over a span of DER. */
typedef struct {
    const uint8_t *p;
    const uint8_t *end;
} der_t;

/* Reads one TLV. On success the cursor moves past it, and the caller gets the
 * tag, the value span and the full TLV span (for hashing and comparing). */
static int der_next(der_t *r, uint8_t *tag, const uint8_t **val, size_t *vlen,
                    const uint8_t **tlv, size_t *tlv_len) {
    if (r->p >= r->end || r->end - r->p < 2) return 0;
    const uint8_t *start = r->p;
    uint8_t t = start[0];
    size_t len = start[1];
    size_t hdr = 2;
    if (len & 0x80u) {
        size_t nb = len & 0x7fu;
        /* DER: long form, minimal, and never more than four length bytes here. */
        if (nb == 0 || nb > 4 || (size_t)(r->end - start) < 2 + nb) return 0;
        len = 0;
        for (size_t i = 0; i < nb; i++) len = (len << 8) | start[2 + i];
        if (len < 0x80u) return 0;
        hdr = 2 + nb;
    }
    if ((size_t)(r->end - start) - hdr < len) return 0;
    *tag = t;
    *val = start + hdr;
    *vlen = len;
    *tlv = start;
    *tlv_len = hdr + len;
    r->p = start + hdr + len;
    return 1;
}

/* Reads the next TLV and requires a particular tag. */
static int der_expect(der_t *r, uint8_t want, const uint8_t **val, size_t *vlen) {
    uint8_t t;
    const uint8_t *tlv;
    size_t tlv_len;
    if (!der_next(r, &t, val, vlen, &tlv, &tlv_len)) return 0;
    return t == want;
}

/* Like der_expect but also returns the whole TLV. */
static int der_expect_tlv(der_t *r, uint8_t want, const uint8_t **val, size_t *vlen,
                          const uint8_t **tlv, size_t *tlv_len) {
    uint8_t t;
    if (!der_next(r, &t, val, vlen, tlv, tlv_len)) return 0;
    return t == want;
}

/* Peeks at the next tag without consuming it. */
static int der_peek(const der_t *r, uint8_t *tag) {
    if (r->p >= r->end) return 0;
    *tag = r->p[0];
    return 1;
}

/* ---- Object identifiers -------------------------------------------------- */

/* Each OID is the DER content octets (no tag or length). */
static const uint8_t OID_RSA_ENC[]       = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01 };
static const uint8_t OID_SHA256_RSA[]    = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0b };
static const uint8_t OID_SHA384_RSA[]    = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0c };
static const uint8_t OID_SHA512_RSA[]    = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0d };
static const uint8_t OID_EC_PUBKEY[]     = { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01 };
static const uint8_t OID_P256[]          = { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07 };
static const uint8_t OID_P384[]          = { 0x2b, 0x81, 0x04, 0x00, 0x22 };
static const uint8_t OID_ECDSA_SHA256[]  = { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02 };
static const uint8_t OID_ECDSA_SHA384[]  = { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x03 };
static const uint8_t OID_BASIC_CONSTR[]  = { 0x55, 0x1d, 0x13 };
static const uint8_t OID_SAN[]           = { 0x55, 0x1d, 0x11 };

static int oid_is(const uint8_t *v, size_t n, const uint8_t *want, size_t want_len) {
    return n == want_len && memcmp(v, want, n) == 0;
}

#define OID_IS(v, n, name) oid_is((v), (n), name, sizeof name)

/* Signature algorithm identifier -> enum. Any other OID is X509_SIG_OTHER. */
static x509_sig_t sig_from_oid(const uint8_t *v, size_t n) {
    if (OID_IS(v, n, OID_SHA256_RSA)) return X509_SIG_RSA_SHA256;
    if (OID_IS(v, n, OID_SHA384_RSA)) return X509_SIG_RSA_SHA384;
    if (OID_IS(v, n, OID_SHA512_RSA)) return X509_SIG_RSA_SHA512;
    if (OID_IS(v, n, OID_ECDSA_SHA256)) return X509_SIG_ECDSA_SHA256;
    if (OID_IS(v, n, OID_ECDSA_SHA384)) return X509_SIG_ECDSA_SHA384;
    return X509_SIG_OTHER;
}

/* ---- Times ---------------------------------------------------------------- */

/* Days from 1970-01-01 to the given civil date (Howard Hinnant's algorithm). */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? (unsigned)-3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static int two(const uint8_t *p, unsigned *out) {
    if (p[0] < '0' || p[0] > '9' || p[1] < '0' || p[1] > '9') return 0;
    *out = (unsigned)(p[0] - '0') * 10u + (unsigned)(p[1] - '0');
    return 1;
}

/* UTCTime (YYMMDDHHMMSSZ) or GeneralizedTime (YYYYMMDDHHMMSSZ). */
static int parse_time(uint8_t tag, const uint8_t *v, size_t n, int64_t *out) {
    unsigned year, mon, day, hh, mm, ss;
    const uint8_t *p = v;
    if (tag == 0x17) {               /* UTCTime: two-digit year, RFC 5280 4.1.2.5.1 */
        if (n != 13 || v[12] != 'Z') return 0;
        if (!two(p, &year)) return 0;
        year += year < 50 ? 2000u : 1900u;
        p += 2;
    } else if (tag == 0x18) {        /* GeneralizedTime */
        if (n != 15 || v[14] != 'Z') return 0;
        unsigned hi;
        if (!two(p, &hi) || !two(p + 2, &year)) return 0;
        year += hi * 100u;
        p += 4;
    } else {
        return 0;
    }
    if (!two(p, &mon) || !two(p + 2, &day) || !two(p + 4, &hh) ||
        !two(p + 6, &mm) || !two(p + 8, &ss)) return 0;
    if (mon < 1 || mon > 12 || day < 1 || day > 31 || hh > 23 || mm > 59 || ss > 60) return 0;
    *out = days_from_civil((int64_t)year, mon, day) * 86400 + (int64_t)hh * 3600 +
           (int64_t)mm * 60 + (int64_t)ss;
    return 1;
}

/* ---- Public key ------------------------------------------------------------ */

/* SubjectPublicKeyInfo: algorithm, then the key BIT STRING. */
static int parse_spki(x509_cert_t *c, const uint8_t *v, size_t n) {
    der_t r = { v, v + n };
    const uint8_t *alg;
    size_t alg_len;
    if (!der_expect(&r, 0x30, &alg, &alg_len)) return 0;

    der_t a = { alg, alg + alg_len };
    const uint8_t *oid;
    size_t oid_len;
    if (!der_expect(&a, 0x06, &oid, &oid_len)) return 0;

    const uint8_t *bits;
    size_t bits_len;
    if (!der_expect(&r, 0x03, &bits, &bits_len) || bits_len < 2 || bits[0] != 0) return 0;
    const uint8_t *key = bits + 1;
    size_t key_len = bits_len - 1;

    if (OID_IS(oid, oid_len, OID_RSA_ENC)) {
        /* RSAPublicKey ::= SEQUENCE { modulus INTEGER, publicExponent INTEGER } */
        der_t k = { key, key + key_len };
        const uint8_t *nv, *ev;
        size_t nl, el;
        if (!der_expect(&k, 0x30, &nv, &nl)) return 0;
        der_t kk = { nv, nv + nl };
        if (!der_expect(&kk, 0x02, &nv, &nl) || !der_expect(&kk, 0x02, &ev, &el)) return 0;
        /* Drop the sign byte that DER adds when the top bit is set. */
        if (nl > 0 && nv[0] == 0) { nv++; nl--; }
        if (nl == 0 || el == 0) return 0;
        c->key_type = X509_KEY_RSA;
        c->rsa_n = nv;
        c->rsa_n_len = nl;
        c->rsa_e = ev;
        c->rsa_e_len = el;
        return 1;
    }
    if (OID_IS(oid, oid_len, OID_EC_PUBKEY)) {
        /* The named curve is the parameters of the algorithm identifier. */
        const uint8_t *curve;
        size_t curve_len;
        if (!der_expect(&a, 0x06, &curve, &curve_len)) return 0;
        if (OID_IS(curve, curve_len, OID_P256)) {
            if (key_len != 65) return 0;
            c->key_type = X509_KEY_EC_P256;
        } else if (OID_IS(curve, curve_len, OID_P384)) {
            if (key_len != 97) return 0;
            c->key_type = X509_KEY_EC_P384;
        } else {
            c->key_type = X509_KEY_OTHER;
            return 1;
        }
        if (key[0] != 0x04) return 0;     /* only uncompressed points are accepted */
        c->ec_point = key;
        c->ec_point_len = key_len;
        return 1;
    }
    c->key_type = X509_KEY_OTHER;
    return 1;
}

/* ---- Extensions ------------------------------------------------------------- */

static int parse_basic_constraints(x509_cert_t *c, const uint8_t *v, size_t n) {
    der_t r = { v, v + n };
    const uint8_t *bv;
    size_t bl;
    if (!der_expect(&r, 0x30, &bv, &bl)) return 0;
    der_t b = { bv, bv + bl };
    c->has_basic_constraints = 1;
    c->is_ca = 0;
    uint8_t tag;
    if (der_peek(&b, &tag) && tag == 0x01) {        /* cA BOOLEAN DEFAULT FALSE */
        const uint8_t *cv;
        size_t cl;
        if (!der_expect(&b, 0x01, &cv, &cl) || cl != 1) return 0;
        c->is_ca = cv[0] != 0;
    }
    return 1;
}

static int parse_san(x509_cert_t *c, const uint8_t *v, size_t n) {
    der_t r = { v, v + n };
    const uint8_t *sv;
    size_t sl;
    if (!der_expect(&r, 0x30, &sv, &sl)) return 0;
    der_t s = { sv, sv + sl };
    while (s.p < s.end) {
        uint8_t t;
        const uint8_t *gv;
        size_t gl;
        const uint8_t *tlv;
        size_t tlv_len;
        if (!der_next(&s, &t, &gv, &gl, &tlv, &tlv_len)) return 0;
        if (t == 0x82 && c->san_count < X509_MAX_SAN) {     /* dNSName [2] IA5String */
            c->san[c->san_count] = gv;
            c->san_len[c->san_count] = gl;
            c->san_count++;
        }
    }
    return 1;
}

static int parse_extensions(x509_cert_t *c, const uint8_t *v, size_t n) {
    der_t r = { v, v + n };
    const uint8_t *ev;
    size_t el;
    if (!der_expect(&r, 0x30, &ev, &el)) return 0;
    der_t e = { ev, ev + el };
    while (e.p < e.end) {
        const uint8_t *xv;
        size_t xl;
        if (!der_expect(&e, 0x30, &xv, &xl)) return 0;
        der_t x = { xv, xv + xl };
        const uint8_t *oid;
        size_t oid_len;
        if (!der_expect(&x, 0x06, &oid, &oid_len)) return 0;
        uint8_t t;
        const uint8_t *cv;
        size_t cl;
        const uint8_t *tlv;
        size_t tlv_len;
        if (!der_peek(&x, &t)) return 0;
        if (t == 0x01) {                                  /* critical BOOLEAN: not needed here */
            if (!der_expect(&x, 0x01, &cv, &cl)) return 0;
        }
        if (!der_expect(&x, 0x04, &cv, &cl)) return 0;    /* extnValue OCTET STRING */
        (void)tlv;
        (void)tlv_len;
        if (OID_IS(oid, oid_len, OID_BASIC_CONSTR)) {
            if (!parse_basic_constraints(c, cv, cl)) return 0;
        } else if (OID_IS(oid, oid_len, OID_SAN)) {
            if (!parse_san(c, cv, cl)) return 0;
        }
    }
    return 1;
}

/* ---- Certificate ------------------------------------------------------------- */

int x509_parse(x509_cert_t *c, const uint8_t *der, size_t len) {
    memset(c, 0, sizeof *c);

    der_t top = { der, der + len };
    const uint8_t *cert_v;
    size_t cert_l;
    if (!der_expect(&top, 0x30, &cert_v, &cert_l) || top.p != top.end) return 0;

    der_t r = { cert_v, cert_v + cert_l };
    const uint8_t *tbs_v, *tbs_tlv;
    size_t tbs_vl, tbs_tlv_len;
    if (!der_expect_tlv(&r, 0x30, &tbs_v, &tbs_vl, &tbs_tlv, &tbs_tlv_len)) return 0;
    c->tbs = tbs_tlv;
    c->tbs_len = tbs_tlv_len;

    /* signatureAlgorithm (outer) */
    const uint8_t *outer_alg;
    size_t outer_alg_len;
    if (!der_expect(&r, 0x30, &outer_alg, &outer_alg_len)) return 0;
    der_t oa = { outer_alg, outer_alg + outer_alg_len };
    const uint8_t *outer_oid;
    size_t outer_oid_len;
    if (!der_expect(&oa, 0x06, &outer_oid, &outer_oid_len)) return 0;
    c->sig_alg = sig_from_oid(outer_oid, outer_oid_len);

    /* signatureValue BIT STRING */
    const uint8_t *sig_v;
    size_t sig_l;
    if (!der_expect(&r, 0x03, &sig_v, &sig_l) || sig_l < 2 || sig_v[0] != 0) return 0;
    c->sig = sig_v + 1;
    c->sig_len = sig_l - 1;
    if (r.p != r.end) return 0;

    /* tbsCertificate fields */
    der_t t = { tbs_v, tbs_v + tbs_vl };
    uint8_t tag;
    const uint8_t *v;
    size_t vl;
    if (!der_peek(&t, &tag)) return 0;
    if (tag == 0xa0) {                                    /* [0] version */
        if (!der_expect(&t, 0xa0, &v, &vl)) return 0;
    }
    if (!der_expect(&t, 0x02, &v, &vl)) return 0;         /* serialNumber */

    /* The inner signature algorithm must match the outer one (RFC 5280 4.1.1.2). */
    const uint8_t *inner_alg;
    size_t inner_alg_len;
    if (!der_expect(&t, 0x30, &inner_alg, &inner_alg_len)) return 0;
    der_t ia = { inner_alg, inner_alg + inner_alg_len };
    const uint8_t *inner_oid;
    size_t inner_oid_len;
    if (!der_expect(&ia, 0x06, &inner_oid, &inner_oid_len)) return 0;
    if (!oid_is(inner_oid, inner_oid_len, outer_oid, outer_oid_len)) return 0;

    /* issuer Name, kept whole */
    const uint8_t *issuer_tlv;
    size_t issuer_tlv_len;
    if (!der_expect_tlv(&t, 0x30, &v, &vl, &issuer_tlv, &issuer_tlv_len)) return 0;
    c->issuer = issuer_tlv;
    c->issuer_len = issuer_tlv_len;

    /* validity */
    const uint8_t *val_v;
    size_t val_l;
    if (!der_expect(&t, 0x30, &val_v, &val_l)) return 0;
    der_t vr = { val_v, val_v + val_l };
    const uint8_t *tv;
    size_t tl;
    uint8_t ttag;
    const uint8_t *tlv2;
    size_t tlv2_len;
    if (!der_next(&vr, &ttag, &tv, &tl, &tlv2, &tlv2_len)) return 0;
    if (!parse_time(ttag, tv, tl, &c->not_before)) return 0;
    if (!der_next(&vr, &ttag, &tv, &tl, &tlv2, &tlv2_len)) return 0;
    if (!parse_time(ttag, tv, tl, &c->not_after)) return 0;
    if (vr.p != vr.end || c->not_after < c->not_before) return 0;

    /* subject Name, kept whole */
    const uint8_t *subj_tlv;
    size_t subj_tlv_len;
    if (!der_expect_tlv(&t, 0x30, &v, &vl, &subj_tlv, &subj_tlv_len)) return 0;
    c->subject = subj_tlv;
    c->subject_len = subj_tlv_len;

    /* subjectPublicKeyInfo */
    if (!der_expect(&t, 0x30, &v, &vl)) return 0;
    if (!parse_spki(c, v, vl)) return 0;

    /* Optional issuerUniqueID [1], subjectUniqueID [2], then extensions [3]. */
    while (t.p < t.end) {
        if (!der_peek(&t, &tag)) return 0;
        if (tag == 0xa3) {
            const uint8_t *ex_v;
            size_t ex_l;
            if (!der_expect(&t, 0xa3, &ex_v, &ex_l)) return 0;
            if (!parse_extensions(c, ex_v, ex_l)) return 0;
        } else if (tag == 0xa1 || tag == 0xa2) {
            if (!der_expect(&t, tag, &v, &vl)) return 0;
        } else {
            return 0;
        }
    }
    return 1;
}
