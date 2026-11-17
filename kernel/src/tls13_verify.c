/* tls13_verify.c — the server-authentication half of the TLS 1.3 handshake (RFC 8446
 * section 4.4). The client has already checked the server Finished; here we check who
 * the server is: its certificate chain, and its CertificateVerify over the transcript. */
#include "tls13_verify.h"
#include "ecdsa_p256.h"
#include "kstring.h"
#include "rsa.h"
#include "sha256.h"
#include "x509.h"

#define HS_CERTIFICATE  11u
#define HS_CERT_VERIFY  15u
#define SCHEME_RSA_PSS_RSAE_SHA256 0x0804u
#define SCHEME_ECDSA_SECP256R1_SHA256 0x0403u

static int fail(char *err, size_t cap, const char *why) {
    if (err && cap) {
        size_t n = strlen(why);
        if (n >= cap) n = cap - 1;
        memcpy(err, why, n);
        err[n] = '\0';
    }
    return 0;
}

static uint32_t be24(const uint8_t *p) {
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

static uint32_t be16(const uint8_t *p) {
    return ((uint32_t)p[0] << 8) | p[1];
}

/* Splits a Certificate message (RFC 8446 section 4.4.2) into DER certificates. The
 * request context is skipped; the per-certificate extensions are ignored. */
static int parse_certificates(const uint8_t *msg, size_t len,
                              const uint8_t *certs[], size_t lens[], size_t max, size_t *count,
                              char *err, size_t cap) {
    if (len < 4 || msg[0] != HS_CERTIFICATE) return fail(err, cap, "no Certificate message");
    if (be24(msg + 1) != len - 4) return fail(err, cap, "bad Certificate length");
    const uint8_t *p = msg + 4, *end = msg + len;
    if (p >= end) return fail(err, cap, "truncated Certificate");
    size_t ctx_len = *p++;
    if ((size_t)(end - p) < ctx_len) return fail(err, cap, "truncated Certificate");
    p += ctx_len;
    if (end - p < 3) return fail(err, cap, "truncated Certificate");
    size_t list_len = be24(p);
    p += 3;
    if ((size_t)(end - p) != list_len) return fail(err, cap, "bad certificate list length");

    *count = 0;
    while (p < end) {
        if (end - p < 3) return fail(err, cap, "truncated certificate entry");
        size_t cl = be24(p);
        p += 3;
        if ((size_t)(end - p) < cl + 2) return fail(err, cap, "truncated certificate entry");
        if (*count >= max) return fail(err, cap, "certificate chain too long");
        certs[*count] = p;
        lens[*count] = cl;
        (*count)++;
        p += cl;
        size_t ext = be16(p);
        p += 2;
        if ((size_t)(end - p) < ext) return fail(err, cap, "truncated extensions");
        p += ext;
    }
    return *count > 0 ? 1 : fail(err, cap, "empty certificate chain");
}

/* Verifies the CertificateVerify signature over the transcript (RFC 8446 section 4.4.3).
 * The signed content is 64 spaces, a context string, a zero byte and the transcript hash. */
static int verify_certificate_verify(const tls13_client_t *c, const x509_cert_t *leaf,
                                     char *err, size_t cap) {
    size_t cv_len;
    const uint8_t *cv = tls13_client_cv_msg(c, &cv_len);
    if (cv_len < 4 + 4 || cv[0] != HS_CERT_VERIFY) return fail(err, cap, "no CertificateVerify message");
    if (be24(cv + 1) != cv_len - 4) return fail(err, cap, "bad CertificateVerify length");
    uint32_t scheme = be16(cv + 4);
    uint32_t sig_len = be16(cv + 6);
    if ((size_t)(cv_len - 8) != sig_len) return fail(err, cap, "bad CertificateVerify signature length");
    const uint8_t *sig = cv + 8;

    static const char context[] = "TLS 1.3, server CertificateVerify";
    uint8_t content[64 + sizeof context + 32];
    memset(content, 0x20, 64);
    memcpy(content + 64, context, sizeof context - 1);
    content[64 + sizeof context - 1] = 0x00;
    memcpy(content + 64 + sizeof context, tls13_client_cv_transcript(c), 32);
    size_t content_len = 64 + sizeof context + 32;

    uint8_t digest[SHA256_DIGEST_LEN];
    sha256(content, content_len, digest);

    switch (scheme) {
    case SCHEME_RSA_PSS_RSAE_SHA256:
        if (leaf->key_type != X509_KEY_RSA) return fail(err, cap, "scheme does not match the certificate key");
        if (!rsa_verify_pss_sha256(leaf->rsa_n, leaf->rsa_n_len, leaf->rsa_e, leaf->rsa_e_len,
                                   digest, sig, sig_len))
            return fail(err, cap, "bad server signature (RSA-PSS)");
        return 1;
    case SCHEME_ECDSA_SECP256R1_SHA256:
        if (leaf->key_type != X509_KEY_EC_P256) return fail(err, cap, "scheme does not match the certificate key");
        if (!ecdsa_p256_verify_sha256(leaf->ec_point, leaf->ec_point_len, digest, sig, sig_len))
            return fail(err, cap, "bad server signature (ECDSA)");
        return 1;
    default:
        return fail(err, cap, "unsupported signature scheme");
    }
}

int tls13_client_verify_server(tls13_client_t *c, const x509_policy_t *pol,
                               char *err, size_t err_cap) {
    if (c->state != TLS_ST_WAIT_AUTH) return fail(err, err_cap, "handshake not finished");

    size_t cert_len;
    const uint8_t *cert_msg = tls13_client_cert_msg(c, &cert_len);
    const uint8_t *certs[X509_VERIFY_MAX_CHAIN];
    size_t lens[X509_VERIFY_MAX_CHAIN];
    size_t count = 0;
    if (!parse_certificates(cert_msg, cert_len, certs, lens, X509_VERIFY_MAX_CHAIN, &count, err, err_cap))
        return 0;

    if (!x509_verify_chain(certs, lens, count, pol, err, err_cap)) return 0;

    /* The chain is trusted; the leaf key must also have signed this handshake. */
    x509_cert_t leaf;
    if (!x509_parse(&leaf, certs[0], lens[0])) return fail(err, err_cap, "malformed leaf certificate");
    if (!verify_certificate_verify(c, &leaf, err, err_cap)) return 0;

    return tls13_client_authenticate(c);
}
