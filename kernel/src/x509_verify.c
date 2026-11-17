/* x509_verify.c — chain checks for TLS. The order matters: cheap structural checks run
 * first, and signatures (the expensive part) run only once the names and times agree. */
#include "x509_verify.h"
#include "ecdsa_p256.h"
#include "kstring.h"
#include "rsa.h"
#include "sha256.h"
#include "x509.h"

static int fail(char *err, size_t cap, const char *why) {
    if (err && cap) {
        size_t n = strlen(why);
        if (n >= cap) n = cap - 1;
        memcpy(err, why, n);
        err[n] = '\0';
    }
    return 0;
}

static int is_ca(const x509_cert_t *c) {
    return c->has_basic_constraints && c->is_ca;
}

static int same_bytes(const uint8_t *a, size_t al, const uint8_t *b, size_t bl) {
    return al == bl && memcmp(a, b, al) == 0;
}

/* Case-insensitive ASCII compare for DNS names. */
static int ascii_eq_ci(const uint8_t *a, size_t al, const char *b, size_t bl) {
    if (al != bl) return 0;
    for (size_t i = 0; i < al; i++) {
        uint8_t x = a[i], y = (uint8_t)b[i];
        if (x >= 'A' && x <= 'Z') x = (uint8_t)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (uint8_t)(y + 32);
        if (x != y) return 0;
    }
    return 1;
}

/* A SAN entry matches the host exactly, or as a wildcard covering one label only:
 * "*.example.test" matches "www.example.test" but not "example.test" or "a.b.example.test". */
static int san_matches(const uint8_t *pat, size_t pat_len, const char *host, size_t host_len) {
    if (pat_len >= 2 && pat[0] == '*' && pat[1] == '.') {
        const char *dot = NULL;
        for (size_t i = 0; i < host_len; i++) {
            if (host[i] == '.') { dot = host + i; break; }
        }
        if (dot == NULL || dot == host) return 0;      /* host needs a first label */
        size_t rest = host_len - (size_t)(dot - host);
        return ascii_eq_ci(pat + 1, pat_len - 1, dot, rest);
    }
    return ascii_eq_ci(pat, pat_len, host, host_len);
}

/* Dotted-quad IPv4 literal to 4 bytes. Returns 0 if the text is not one. */
static int parse_ipv4(const char *s, size_t n, uint8_t out[4]) {
    size_t i = 0;
    for (int part = 0; part < 4; part++) {
        if (i >= n || s[i] < '0' || s[i] > '9') return 0;
        unsigned v = 0;
        size_t digits = 0;
        while (i < n && s[i] >= '0' && s[i] <= '9') {
            v = v * 10u + (unsigned)(s[i] - '0');
            if (++digits > 3 || v > 255u) return 0;
            i++;
        }
        out[part] = (uint8_t)v;
        if (part < 3) {
            if (i >= n || s[i] != '.') return 0;
            i++;
        }
    }
    return i == n;
}

/* An IP address host must match an iPAddress SAN exactly; a name must match a dNSName.
 * The two never match each other. */
static int host_matches_leaf(const x509_cert_t *leaf, const char *host, size_t host_len) {
    uint8_t ip[4];
    int is_ip = parse_ipv4(host, host_len, ip);
    for (int i = 0; i < leaf->san_count; i++) {
        if (is_ip) {
            if (leaf->san_kind[i] == 0x87 && leaf->san_len[i] == 4 &&
                memcmp(leaf->san[i], ip, 4) == 0)
                return 1;
        } else if (leaf->san_kind[i] == 0x82 &&
                   san_matches(leaf->san[i], leaf->san_len[i], host, host_len)) {
            return 1;
        }
    }
    return 0;
}

/* Does `issuer` sign `subject`? Returns 1, or 0 with a reason. Only the two algorithms
 * implemented in this kernel are accepted; anything else is refused by name. */
static int signed_by(const x509_cert_t *subject, const x509_cert_t *issuer,
                     char *err, size_t cap) {
    uint8_t digest[SHA256_DIGEST_LEN];
    switch (subject->sig_alg) {
    case X509_SIG_RSA_SHA256:
        if (issuer->key_type != X509_KEY_RSA)
            return fail(err, cap, "signature algorithm does not match the issuer key");
        sha256(subject->tbs, subject->tbs_len, digest);
        if (!rsa_verify_pkcs1_sha256(issuer->rsa_n, issuer->rsa_n_len, issuer->rsa_e,
                                     issuer->rsa_e_len, digest, subject->sig, subject->sig_len))
            return fail(err, cap, "bad RSA signature");
        return 1;
    case X509_SIG_ECDSA_SHA256:
        if (issuer->key_type != X509_KEY_EC_P256)
            return fail(err, cap, "signature algorithm does not match the issuer key");
        sha256(subject->tbs, subject->tbs_len, digest);
        if (!ecdsa_p256_verify_sha256(issuer->ec_point, issuer->ec_point_len, digest,
                                      subject->sig, subject->sig_len))
            return fail(err, cap, "bad ECDSA signature");
        return 1;
    case X509_SIG_RSA_SHA384:
    case X509_SIG_RSA_SHA512:
    case X509_SIG_ECDSA_SHA384:
        return fail(err, cap, "signature algorithm not supported yet (needs SHA-384 or SHA-512)");
    default:
        return fail(err, cap, "unknown signature algorithm");
    }
}

/* Finds the trust anchor that issued `top` when `top` is not itself a trusted root.
 * Returns 1 when some root's subject equals top's issuer and that root's key verifies top. */
static int anchored_by_root(const x509_cert_t *top, const x509_policy_t *pol,
                            char *err, size_t cap) {
    /* Keep the reason from the closest root (the one with the right name), so a bad
     * signature is reported as one and not as a generic "no root". */
    char reason[96] = "no trusted root for the chain's issuer";
    for (size_t i = 0; i < pol->root_count; i++) {
        x509_cert_t root;
        if (!x509_parse(&root, pol->roots[i].der, pol->roots[i].len)) continue;
        if (!same_bytes(root.subject, root.subject_len, top->issuer, top->issuer_len)) continue;
        if (!is_ca(&root)) { fail(reason, sizeof reason, "trust anchor is not a CA"); continue; }
        if (pol->now < root.not_before || pol->now > root.not_after) {
            fail(reason, sizeof reason, "trust anchor expired or not yet valid");
            continue;
        }
        if (signed_by(top, &root, reason, sizeof reason)) return 1;
    }
    return fail(err, cap, reason);
}

int x509_verify_chain(const uint8_t *const certs[], const size_t lens[], size_t count,
                      const x509_policy_t *pol, char *err, size_t err_cap) {
    if (count == 0 || count > X509_VERIFY_MAX_CHAIN) return fail(err, err_cap, "chain length");

    /* Parse every certificate first; a malformed one anywhere fails the whole chain. */
    x509_cert_t cert[X509_VERIFY_MAX_CHAIN];
    for (size_t i = 0; i < count; i++) {
        if (!x509_parse(&cert[i], certs[i], lens[i])) return fail(err, err_cap, "malformed certificate");
    }

    /* The leaf must be an end-entity certificate that names the host. */
    if (is_ca(&cert[0])) return fail(err, err_cap, "leaf is a CA certificate");
    if (!host_matches_leaf(&cert[0], pol->host, pol->host_len))
        return fail(err, err_cap, "certificate does not name this host");

    /* Validity period for every certificate in the chain. */
    for (size_t i = 0; i < count; i++) {
        if (pol->now < cert[i].not_before) return fail(err, err_cap, "certificate not yet valid");
        if (pol->now > cert[i].not_after) return fail(err, err_cap, "certificate expired");
    }

    /* Each certificate is signed by the next one, which must be a CA with the same name. */
    for (size_t i = 0; i + 1 < count; i++) {
        const x509_cert_t *issuer = &cert[i + 1];
        if (!same_bytes(cert[i].issuer, cert[i].issuer_len, issuer->subject, issuer->subject_len))
            return fail(err, err_cap, "issuer name does not match the next certificate");
        if (!is_ca(issuer)) return fail(err, err_cap, "issuer is not a CA");
        if (!signed_by(&cert[i], issuer, err, err_cap)) return 0;
    }

    /* The top of the chain is trusted if it is a built-in root, or if a built-in root
     * signed it. A self-signed certificate that is not a root is refused. */
    const x509_cert_t *top = &cert[count - 1];
    for (size_t i = 0; i < pol->root_count; i++) {
        if (same_bytes(pol->roots[i].der, pol->roots[i].len, certs[count - 1], lens[count - 1]))
            return 1;
    }
    return anchored_by_root(top, pol, err, err_cap);
}
