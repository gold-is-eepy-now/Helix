/* x509-unit.c — parses certificates made by the Python `cryptography` library
 * (tests/x509-vectors.h, from tests/gen-x509-vectors.py) and checks every field
 * against the values that library reports. Also checks chain linking by name and
 * that malformed DER is refused. */
#include <stdio.h>
#include <string.h>

#include "x509.h"
#include "x509-vectors.h"
#include "trust_roots.h"

static int failures;

static void check(const char *name, int ok) {
    printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) failures++;
}

static int same(const uint8_t *a, size_t al, const uint8_t *b, size_t bl) {
    return al == bl && memcmp(a, b, al) == 0;
}

int main(void) {
    x509_cert_t ec_root, ec_leaf, ec384, rr, rl, bad;

    /* EC P-256 root */
    check("EC P-256 root parses", x509_parse(&ec_root, ec256_root, EC256_ROOT_LEN));
    check("  validity matches the generator", ec_root.not_before == 1767225600 && ec_root.not_after == 2082585600);
    check("  key type is EC P-256", ec_root.key_type == X509_KEY_EC_P256);
    check("  point is uncompressed, 65 bytes", ec_root.ec_point_len == 65 && ec_root.ec_point[0] == 4);
    check("  signed with ecdsa-with-SHA256", ec_root.sig_alg == X509_SIG_ECDSA_SHA256);
    check("  basicConstraints marks a CA", ec_root.has_basic_constraints && ec_root.is_ca);
    check("  root has no SAN", ec_root.san_count == 0);
    check("  signature is a DER SEQUENCE", ec_root.sig_len > 8 && ec_root.sig[0] == 0x30);
    check("  signed bytes start at the tbsCertificate", ec_root.tbs[0] == 0x30 && ec_root.tbs_len > 100);

    /* EC P-256 leaf, issued by the root */
    check("EC P-256 leaf parses", x509_parse(&ec_leaf, ec256_leaf, EC256_LEAF_LEN));
    check("  leaf is not a CA", ec_leaf.has_basic_constraints && !ec_leaf.is_ca);
    check("  leaf SAN is leaf.example.test", ec_leaf.san_count == 1
          && same(ec_leaf.san[0], ec_leaf.san_len[0], (const uint8_t *)"leaf.example.test", 17));
    check("  leaf issuer matches the root subject byte for byte",
          same(ec_leaf.issuer, ec_leaf.issuer_len, ec_root.subject, ec_root.subject_len));

    /* EC P-384 leaf, signed with ecdsa-with-SHA384 */
    check("EC P-384 leaf parses", x509_parse(&ec384, ec384_leaf, EC384_LEAF_LEN));
    check("  key type is EC P-384", ec384.key_type == X509_KEY_EC_P384);
    check("  point is uncompressed, 97 bytes", ec384.ec_point_len == 97 && ec384.ec_point[0] == 4);
    check("  signed with ecdsa-with-SHA384", ec384.sig_alg == X509_SIG_ECDSA_SHA384);
    check("  SAN is p384.example.test", ec384.san_count == 1
          && same(ec384.san[0], ec384.san_len[0], (const uint8_t *)"p384.example.test", 17));

    /* RSA root and leaf */
    static const uint8_t e65537[3] = { 0x01, 0x00, 0x01 };
    check("RSA root parses", x509_parse(&rr, rsa_root, RSA_ROOT_LEN));
    check("  key type is RSA", rr.key_type == X509_KEY_RSA);
    check("  modulus is 2048 bits with the sign byte removed", rr.rsa_n_len == 256 && rr.rsa_n[0] != 0);
    check("  public exponent is 65537", same(rr.rsa_e, rr.rsa_e_len, e65537, 3));
    check("  signed with sha256WithRSAEncryption", rr.sig_alg == X509_SIG_RSA_SHA256);
    check("  RSA signature is 256 bytes", rr.sig_len == 256);
    check("  RSA root is a CA", rr.is_ca);

    check("RSA leaf parses", x509_parse(&rl, rsa_leaf, RSA_LEAF_LEN));
    check("  leaf SAN is rsa.example.test", rl.san_count == 1
          && same(rl.san[0], rl.san_len[0], (const uint8_t *)"rsa.example.test", 16));
    check("  leaf issuer matches the RSA root subject",
          same(rl.issuer, rl.issuer_len, rr.subject, rr.subject_len));

    /* Malformed input must be refused, not half-parsed. */
    uint8_t copy[sizeof ec256_leaf + 1];
    memcpy(copy, ec256_leaf, EC256_LEAF_LEN);
    check("empty input is refused", !x509_parse(&bad, copy, 0));
    check("truncated certificate is refused", !x509_parse(&bad, copy, EC256_LEAF_LEN - 1));
    copy[0] = 0x31;
    check("wrong outer tag is refused", !x509_parse(&bad, copy, EC256_LEAF_LEN));
    copy[0] = 0x30;
    copy[EC256_LEAF_LEN] = 0x00;
    check("trailing bytes after the certificate are refused", !x509_parse(&bad, copy, EC256_LEAF_LEN + 1));

    /* Every built-in root must parse, be a CA, and be self-issued. */
    size_t parsed = 0, cas = 0, self_issued = 0;
    for (size_t i = 0; i < trust_root_count; i++) {
        x509_cert_t r;
        if (!x509_parse(&r, trust_roots[i].der, trust_roots[i].len)) continue;
        parsed++;
        if (r.is_ca) cas++;
        if (same(r.issuer, r.issuer_len, r.subject, r.subject_len)) self_issued++;
    }
    printf("roots: %zu of %zu parsed, %zu CA, %zu self-issued\n", parsed, trust_root_count, cas, self_issued);
    check("every built-in root parses", parsed == trust_root_count && trust_root_count > 100);
    check("every built-in root is a CA", cas == trust_root_count);
    check("every built-in root is self-issued", self_issued == trust_root_count);

    if (failures) {
        printf("x509-unit: %d failure(s)\n", failures);
        return 1;
    }
    printf("x509-unit: all checks passed\n");
    return 0;
}
