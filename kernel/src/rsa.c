/* rsa.c — RSA public-key verification. Both schemes first compute m = s^e mod n, then
 * check the padding. Every check failure returns 0; nothing here returns partial data. */
#include "rsa.h"
#include "bignum.h"
#include "kstring.h"
#include "sha256.h"

#define RSA_MAX_BYTES (BN_MAX_BITS / 8u)

/* DER DigestInfo prefix for SHA-256 (RFC 8017 section 9.2, note 1). The full T is this
 * prefix followed by the 32-byte hash. */
static const uint8_t DIGEST_INFO_SHA256[19] = {
    0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01,
    0x05, 0x00, 0x04, 0x20,
};

/* Computes em = s^e mod n as nlen big-endian bytes. Returns 0 if the signature is the
 * wrong length or not below the modulus (RFC 8017 section 5.2.2 steps 1 and 2). */
static int rsa_public_op(const uint8_t *n, size_t nlen, const uint8_t *e, size_t elen,
                         const uint8_t *sig, size_t siglen, uint8_t em[RSA_MAX_BYTES]) {
    if (nlen == 0 || nlen > RSA_MAX_BYTES || elen == 0) return 0;
    if (siglen != nlen) return 0;

    size_t k = (nlen + 3) / 4;                       /* words in the modulus */
    uint32_t nw[BN_MAX_WORDS], sw[BN_MAX_WORDS], out[BN_MAX_WORDS];
    mont_t m;

    if (!bn_from_be(nw, k, n, nlen)) return 0;
    if (!bn_from_be(sw, k, sig, siglen)) return 0;
    if (bn_cmp(sw, nw, k) >= 0) return 0;            /* signature must be below n */
    if (!mont_init(&m, nw, k)) return 0;             /* n must be odd and at least 3 */

    mont_modexp(&m, out, sw, e, elen);
    bn_to_be(out, k, em, nlen);
    return 1;
}

int rsa_verify_pkcs1_sha256(const uint8_t *n, size_t nlen, const uint8_t *e, size_t elen,
                            const uint8_t hash[32], const uint8_t *sig, size_t siglen) {
    uint8_t em[RSA_MAX_BYTES];
    if (!rsa_public_op(n, nlen, e, elen, sig, siglen, em)) return 0;

    /* EM = 0x00 || 0x01 || PS (0xff, at least 8 bytes) || 0x00 || T. */
    size_t t_len = sizeof DIGEST_INFO_SHA256 + 32;
    if (nlen < t_len + 11) return 0;

    uint8_t expect[RSA_MAX_BYTES];
    size_t ps_len = nlen - t_len - 3;
    expect[0] = 0x00;
    expect[1] = 0x01;
    memset(expect + 2, 0xff, ps_len);
    expect[2 + ps_len] = 0x00;
    memcpy(expect + 3 + ps_len, DIGEST_INFO_SHA256, sizeof DIGEST_INFO_SHA256);
    memcpy(expect + 3 + ps_len + sizeof DIGEST_INFO_SHA256, hash, 32);

    /* The whole block must match exactly, so a forged short or padded value fails. */
    return memcmp(em, expect, nlen) == 0;
}

/* MGF1 with SHA-256 (RFC 8017 appendix B.2.1): concatenated hashes of seed || counter. */
static void mgf1_sha256(const uint8_t *seed, size_t seed_len, uint8_t *mask, size_t mask_len) {
    uint8_t counter[4];
    size_t done = 0;
    for (uint32_t c = 0; done < mask_len; c++) {
        counter[0] = (uint8_t)(c >> 24);
        counter[1] = (uint8_t)(c >> 16);
        counter[2] = (uint8_t)(c >> 8);
        counter[3] = (uint8_t)c;
        sha256_ctx_t ctx;
        sha256_init(&ctx);
        sha256_update(&ctx, seed, seed_len);
        sha256_update(&ctx, counter, sizeof counter);
        uint8_t block[32];
        sha256_final(&ctx, block);
        size_t take = mask_len - done < 32 ? mask_len - done : 32;
        memcpy(mask + done, block, take);
        done += take;
    }
}

int rsa_verify_pss_sha256(const uint8_t *n, size_t nlen, const uint8_t *e, size_t elen,
                          const uint8_t hash[32], const uint8_t *sig, size_t siglen) {
    const size_t h_len = 32, s_len = 32;
    uint8_t em_full[RSA_MAX_BYTES];
    if (!rsa_public_op(n, nlen, e, elen, sig, siglen, em_full)) return 0;

    /* emBits = modBits - 1: the modulus is one bit shorter than its byte length
     * when its top bit is clear, and the encoded message must stay below it. */
    size_t mod_bits = 0;
    for (size_t i = 0; i < nlen; i++) {
        if (n[i] != 0) {
            uint8_t top = n[i];
            size_t bits = 0;
            for (; top != 0; top >>= 1) bits++;
            mod_bits = (nlen - i - 1) * 8 + bits;
            break;
        }
    }
    if (mod_bits < 2) return 0;
    size_t em_bits = mod_bits - 1;
    size_t em_len = (em_bits + 7) / 8;

    /* EM is the rightmost em_len bytes of the value; the bytes above must be zero. */
    if (em_len < h_len + s_len + 2 || em_len > nlen) return 0;
    for (size_t i = 0; i < nlen - em_len; i++) {
        if (em_full[i] != 0) return 0;
    }
    const uint8_t *em = em_full + (nlen - em_len);

    if (em[em_len - 1] != 0xbc) return 0;            /* trailer field */

    size_t db_len = em_len - h_len - 1;
    const uint8_t *masked_db = em;
    const uint8_t *h = em + db_len;

    /* The leftmost 8*em_len - em_bits bits of maskedDB must be zero. */
    uint8_t top_mask = (uint8_t)(0xff >> (8 * em_len - em_bits));
    if (masked_db[0] & ~top_mask) return 0;

    uint8_t db[RSA_MAX_BYTES];
    mgf1_sha256(h, h_len, db, db_len);
    for (size_t i = 0; i < db_len; i++) db[i] ^= masked_db[i];
    db[0] &= top_mask;

    /* DB must be zero padding, then 0x01, then the salt. */
    size_t ps_len = db_len - s_len - 1;
    for (size_t i = 0; i < ps_len; i++) {
        if (db[i] != 0) return 0;
    }
    if (db[ps_len] != 0x01) return 0;
    const uint8_t *salt = db + ps_len + 1;

    /* H' = SHA-256(8 zero bytes || mHash || salt) must equal H. */
    uint8_t m_prime[8 + 32 + 32];
    memset(m_prime, 0, 8);
    memcpy(m_prime + 8, hash, h_len);
    memcpy(m_prime + 8 + h_len, salt, s_len);
    uint8_t h_prime[32];
    sha256(m_prime, sizeof m_prime, h_prime);

    return memcmp(h_prime, h, h_len) == 0;
}
