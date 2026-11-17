/* x509-verify-unit.c — chain checks from tests/x509-vectors.h (made by gen-x509-vectors.py)
 * against custom trust anchors. Good chains must pass; wrong host, time, anchor, algorithm
 * and signature must each be refused, with a reason that names the actual failure. */
#include <stdio.h>
#include <string.h>

#include "x509_verify.h"
#include "x509-vectors.h"
#include "x509-chain-vectors.h"

static int failures;

static void check(const char *name, int ok) {
    printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) failures++;
}

#define NOW 1800000000LL   /* 2027-01-15, inside the vectors' validity */

static int verify(const uint8_t *leaf, size_t leaf_len, const uint8_t *anchor, size_t anchor_len,
                  const char *host, int64_t now, char *err, size_t cap) {
    const uint8_t *certs[1] = { leaf };
    size_t lens[1] = { leaf_len };
    trust_root_t roots[1] = { { anchor, anchor_len } };
    x509_policy_t pol = { now, host, strlen(host), roots, 1 };
    return x509_verify_chain(certs, lens, 1, &pol, err, cap);
}

int main(void) {
    char err[96];

    check("EC P-256 leaf verifies against its root",
          verify(ec256_leaf, EC256_LEAF_LEN, ec256_root, EC256_ROOT_LEN, "leaf.example.test", NOW, err, sizeof err));
    check("RSA leaf verifies against its root",
          verify(rsa_leaf, RSA_LEAF_LEN, rsa_root, RSA_ROOT_LEN, "rsa.example.test", NOW, err, sizeof err));

    /* Name checks */
    check("wrong host is refused",
          !verify(ec256_leaf, EC256_LEAF_LEN, ec256_root, EC256_ROOT_LEN, "other.example.test", NOW, err, sizeof err));
    check("  reason names the host",
          strstr(err, "name") != NULL);
    check("host match ignores ASCII case",
          verify(ec256_leaf, EC256_LEAF_LEN, ec256_root, EC256_ROOT_LEN, "LEAF.Example.Test", NOW, err, sizeof err));

    /* Time checks */
    check("expired certificate is refused",
          !verify(ec256_leaf, EC256_LEAF_LEN, ec256_root, EC256_ROOT_LEN, "leaf.example.test", 2200000000LL, err, sizeof err));
    check("  reason says expired", strstr(err, "expired") != NULL);
    check("not-yet-valid certificate is refused",
          !verify(ec256_leaf, EC256_LEAF_LEN, ec256_root, EC256_ROOT_LEN, "leaf.example.test", 1000, err, sizeof err));
    check("  reason says not yet valid", strstr(err, "not yet valid") != NULL);

    /* Trust checks */
    check("leaf is refused when the anchor is the wrong algorithm",
          !verify(ec256_leaf, EC256_LEAF_LEN, rsa_root, RSA_ROOT_LEN, "leaf.example.test", NOW, err, sizeof err));
    check("a CA certificate is refused as a leaf",
          !verify(ec256_root, EC256_ROOT_LEN, ec256_root, EC256_ROOT_LEN, "leaf.example.test", NOW, err, sizeof err));
    check("  reason says CA", strstr(err, "CA") != NULL);

    /* Signature checks: flip one bit in the last byte of the leaf's signature. */
    uint8_t tampered[EC256_LEAF_LEN];
    memcpy(tampered, ec256_leaf, EC256_LEAF_LEN);
    tampered[EC256_LEAF_LEN - 1] ^= 0x01;
    check("tampered ECDSA signature is refused",
          !verify(tampered, EC256_LEAF_LEN, ec256_root, EC256_ROOT_LEN, "leaf.example.test", NOW, err, sizeof err));
    check("  reason names the signature", strstr(err, "signature") != NULL);

    uint8_t rsa_t[RSA_LEAF_LEN];
    memcpy(rsa_t, rsa_leaf, RSA_LEAF_LEN);
    rsa_t[RSA_LEAF_LEN - 1] ^= 0x01;
    check("tampered RSA signature is refused",
          !verify(rsa_t, RSA_LEAF_LEN, rsa_root, RSA_ROOT_LEN, "rsa.example.test", NOW, err, sizeof err));
    check("  reason names the signature", strstr(err, "signature") != NULL);

    /* Malformed input */
    check("truncated leaf is refused",
          !verify(ec256_leaf, EC256_LEAF_LEN - 5, ec256_root, EC256_ROOT_LEN, "leaf.example.test", NOW, err, sizeof err));

    /* Three-certificate chain: RSA root -> EC intermediate -> EC leaf. */
    const uint8_t *chain[3] = { chain_leaf, chain_inter, chain_leaf };
    size_t chain_lens[3] = { CHAIN_LEAF_LEN, CHAIN_INTER_LEN, CHAIN_LEAF_LEN };
    trust_root_t chain_roots[1] = { { chain_root, CHAIN_ROOT_LEN } };
    x509_policy_t chain_pol = { NOW, "chain.example.test", 18, chain_roots, 1 };
    check("leaf + intermediate verify against the RSA root",
          x509_verify_chain(chain, chain_lens, 2, &chain_pol, err, sizeof err));
    check("  the root alone is not enough when the intermediate is missing",
          !x509_verify_chain(chain, chain_lens, 1, &chain_pol, err, sizeof err));
    check("  reason says the leaf's issuer is unknown or unsigned",
          strstr(err, "trusted root") != NULL || strstr(err, "issuer") != NULL);

    chain_pol.host = "other.example.test";
    chain_pol.host_len = 18;
    check("  wrong host is refused for the chain", !x509_verify_chain(chain, chain_lens, 2, &chain_pol, err, sizeof err));
    chain_pol.host = "chain.example.test";
    chain_pol.host_len = 18;

    uint8_t inter_t[CHAIN_INTER_LEN];
    memcpy(inter_t, chain_inter, CHAIN_INTER_LEN);
    inter_t[CHAIN_INTER_LEN - 1] ^= 0x01;
    const uint8_t *bad_chain[2] = { chain_leaf, inter_t };
    size_t bad_lens[2] = { CHAIN_LEAF_LEN, CHAIN_INTER_LEN };
    check("tampered intermediate signature is refused",
          !x509_verify_chain(bad_chain, bad_lens, 2, &chain_pol, err, sizeof err));
    check("  reason names the signature", strstr(err, "signature") != NULL);

    const uint8_t *swapped[2] = { chain_inter, chain_leaf };
    size_t swapped_lens[2] = { CHAIN_INTER_LEN, CHAIN_LEAF_LEN };
    check("certificates out of order are refused",
          !x509_verify_chain(swapped, swapped_lens, 2, &chain_pol, err, sizeof err));

    /* Intermediate must be a CA: the leaf cannot vouch for another certificate. */
    const uint8_t *leaf_as_issuer[2] = { chain_leaf, chain_leaf };
    size_t leaf_as_issuer_lens[2] = { CHAIN_LEAF_LEN, CHAIN_LEAF_LEN };
    check("a non-CA issuer is refused",
          !x509_verify_chain(leaf_as_issuer, leaf_as_issuer_lens, 2, &chain_pol, err, sizeof err));

    /* IP-address host: the leaf's only SAN is an iPAddress. */
    const uint8_t *ip_chain[2] = { chain_ip_leaf, chain_inter };
    size_t ip_lens[2] = { CHAIN_IP_LEAF_LEN, CHAIN_INTER_LEN };
    x509_policy_t ip_pol = { NOW, "10.0.2.2", 8, chain_roots, 1 };
    check("IP address host matches an iPAddress SAN",
          x509_verify_chain(ip_chain, ip_lens, 2, &ip_pol, err, sizeof err));
    ip_pol.host = "10.0.2.3";
    ip_pol.host_len = 8;
    check("  a neighbouring address is refused", !x509_verify_chain(ip_chain, ip_lens, 2, &ip_pol, err, sizeof err));
    check("  reason names the host", strstr(err, "name") != NULL);
    ip_pol.host = "chain.example.test";
    ip_pol.host_len = 18;
    check("  a DNS name does not match an iPAddress SAN", !x509_verify_chain(ip_chain, ip_lens, 2, &ip_pol, err, sizeof err));
    /* A DNS-only leaf must not be accepted for an address that looks like its name. */
    chain_pol.host = "10.0.2.2";
    chain_pol.host_len = 8;
    check("  a DNS-only leaf is refused for an address", !x509_verify_chain(chain, chain_lens, 2, &chain_pol, err, sizeof err));
    chain_pol.host = "chain.example.test";
    chain_pol.host_len = 18;

    printf("x509-verify-unit: %s (%d failure(s))\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
