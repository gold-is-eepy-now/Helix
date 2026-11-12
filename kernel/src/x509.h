/* x509.h — parses an X.509 v3 certificate (RFC 5280) from DER. This only reads the
 * fields chain checking needs: the signed bytes, names, validity, the public key,
 * the signature, basicConstraints and subjectAltName dNSNames. It does NOT verify
 * signatures; see x509_verify (not yet written). All pointers refer into the
 * caller's DER buffer, which must outlive the parsed structure. */
#ifndef HELIX_X509_H
#define HELIX_X509_H

#include <stddef.h>
#include <stdint.h>

#define X509_MAX_SAN 16

typedef enum {
    X509_KEY_OTHER = 0,
    X509_KEY_RSA,
    X509_KEY_EC_P256,
    X509_KEY_EC_P384,
} x509_key_t;

typedef enum {
    X509_SIG_OTHER = 0,
    X509_SIG_RSA_SHA256,
    X509_SIG_RSA_SHA384,
    X509_SIG_RSA_SHA512,
    X509_SIG_ECDSA_SHA256,
    X509_SIG_ECDSA_SHA384,
} x509_sig_t;

typedef struct {
    /* The signed part: the full tbsCertificate TLV, as hashed for the signature. */
    const uint8_t *tbs;
    size_t tbs_len;

    /* Full Name TLVs, compared byte for byte when chaining issuer to subject. */
    const uint8_t *issuer;
    size_t issuer_len;
    const uint8_t *subject;
    size_t subject_len;

    /* Validity, as seconds since 1970-01-01 UTC. */
    int64_t not_before;
    int64_t not_after;

    x509_key_t key_type;
    const uint8_t *rsa_n;       /* modulus, big-endian, leading zero removed */
    size_t rsa_n_len;
    const uint8_t *rsa_e;       /* public exponent, big-endian */
    size_t rsa_e_len;
    const uint8_t *ec_point;    /* uncompressed point 0x04 || X || Y */
    size_t ec_point_len;

    /* The signature over tbs, made by the issuer. For ECDSA it is the DER
     * SEQUENCE { r, s }; for RSA it is the raw big-endian signature. */
    x509_sig_t sig_alg;
    const uint8_t *sig;
    size_t sig_len;

    int has_basic_constraints;
    int is_ca;

    int san_count;
    const uint8_t *san[X509_MAX_SAN];
    size_t san_len[X509_MAX_SAN];
} x509_cert_t;

/* Returns 1 on success. Fails on malformed DER, on an outer and inner signature
 * algorithm that disagree, and on negative or undecodable times. */
int x509_parse(x509_cert_t *c, const uint8_t *der, size_t len);

#endif /* HELIX_X509_H */
