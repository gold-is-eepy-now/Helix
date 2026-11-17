/* x509_verify.h — checks a certificate chain for TLS: each certificate is inside its
 * validity period, each issuer is a CA whose name matches and whose key verifies the
 * signature, the chain ends at a trusted root, and the leaf names the host.
 *
 * Not checked yet (so the chain is weaker than a full RFC 5280 check): path length
 * constraints, name constraints, key usage and extended key usage, revocation (CRL/OCSP),
 * and certificate policies. Algorithms outside RSA-SHA256 and ECDSA-SHA256 on P-256 are
 * refused, not skipped. */
#ifndef HELIX_X509_VERIFY_H
#define HELIX_X509_VERIFY_H

#include <stddef.h>
#include <stdint.h>

#include "trust_roots.h"

#define X509_VERIFY_MAX_CHAIN 8

typedef struct {
    int64_t now;                          /* seconds since 1970-01-01 UTC */
    const char *host;                     /* the DNS name the leaf must name */
    size_t host_len;
    const trust_root_t *roots;            /* trust anchors (DER), e.g. trust_roots */
    size_t root_count;
} x509_policy_t;

/* certs[0] is the leaf, then the intermediates in the order the server sent them.
 * Returns 1 if the chain is trusted for the host. On failure it returns 0 and writes a
 * short reason into err (never empty on failure). */
int x509_verify_chain(const uint8_t *const certs[], const size_t lens[], size_t count,
                      const x509_policy_t *pol, char *err, size_t err_cap);

#endif /* HELIX_X509_VERIFY_H */
