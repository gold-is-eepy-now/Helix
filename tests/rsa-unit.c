/* rsa-unit.c — checks bignum.c and rsa.c against signatures made by the Python
 * `cryptography` library (tests/rsa-vectors.h, from tests/gen-rsa-vectors.py). Every
 * good signature must verify, and every altered one must be refused. */
#include <stdio.h>
#include <string.h>

#include "bignum.h"
#include "rsa.h"
#include "rsa-vectors.h"

static int failures;

static void check(const char *name, int ok) {
    printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) failures++;
}

static void test_bignum(void) {
    /* 3^5 mod 7 = 243 mod 7 = 5. */
    uint32_t n[1] = { 7 }, base[1] = { 3 }, out[1];
    mont_t m;
    check("mont_init accepts an odd modulus", mont_init(&m, n, 1));
    uint8_t e5[1] = { 5 };
    mont_modexp(&m, out, base, e5, 1);
    check("3^5 mod 7 = 5", out[0] == 5);

    uint32_t even[1] = { 8 };
    check("mont_init refuses an even modulus", !mont_init(&m, even, 1));

    /* Big-endian round trip over a 40-bit value (two words). */
    uint8_t in[5] = { 0x01, 0x23, 0x45, 0x67, 0x89 };
    uint32_t w[2];
    check("bn_from_be loads a 5-byte value", bn_from_be(w, 2, in, 5));
    uint8_t outb[5];
    bn_to_be(w, 2, outb, 5);
    check("bn_to_be returns the same bytes", memcmp(in, outb, 5) == 0);
    uint32_t one_word[1];
    check("bn_from_be refuses a value that does not fit", !bn_from_be(one_word, 1, in, 5));
}

int main(void) {
    test_bignum();

    /* PKCS#1 v1.5 */
    check("PKCS#1 v1.5 signature verifies",
          rsa_verify_pkcs1_sha256(RSA_N, 256, RSA_E, 3, RSA_HASH, RSA_PKCS1_SIG, 256));

    uint8_t bad_hash[32];
    memcpy(bad_hash, RSA_HASH, 32);
    bad_hash[0] ^= 1;
    check("PKCS#1 refuses a different hash",
          !rsa_verify_pkcs1_sha256(RSA_N, 256, RSA_E, 3, bad_hash, RSA_PKCS1_SIG, 256));

    check("PKCS#1 refuses a signature for another message",
          !rsa_verify_pkcs1_sha256(RSA_N, 256, RSA_E, 3, RSA_HASH, RSA_OTHER_SIG, 256));

    uint8_t flipped[256];
    memcpy(flipped, RSA_PKCS1_SIG, 256);
    flipped[100] ^= 0x10;
    check("PKCS#1 refuses a one-bit change in the signature",
          !rsa_verify_pkcs1_sha256(RSA_N, 256, RSA_E, 3, RSA_HASH, flipped, 256));

    check("PKCS#1 refuses a short signature",
          !rsa_verify_pkcs1_sha256(RSA_N, 256, RSA_E, 3, RSA_HASH, RSA_PKCS1_SIG, 255));

    /* PSS */
    check("PSS signature verifies",
          rsa_verify_pss_sha256(RSA_N, 256, RSA_E, 3, RSA_HASH, RSA_PSS_SIG, 256));

    check("PSS refuses a different hash",
          !rsa_verify_pss_sha256(RSA_N, 256, RSA_E, 3, bad_hash, RSA_PSS_SIG, 256));

    memcpy(flipped, RSA_PSS_SIG, 256);
    flipped[200] ^= 0x01;
    check("PSS refuses a one-bit change in the signature",
          !rsa_verify_pss_sha256(RSA_N, 256, RSA_E, 3, RSA_HASH, flipped, 256));

    check("PSS refuses a PKCS#1 signature",
          !rsa_verify_pss_sha256(RSA_N, 256, RSA_E, 3, RSA_HASH, RSA_PKCS1_SIG, 256));

    check("PKCS#1 refuses a PSS signature",
          !rsa_verify_pkcs1_sha256(RSA_N, 256, RSA_E, 3, RSA_HASH, RSA_PSS_SIG, 256));

    /* A signature equal to the modulus is not a valid residue. */
    check("refuses a signature not below the modulus",
          !rsa_verify_pkcs1_sha256(RSA_N, 256, RSA_E, 3, RSA_HASH, RSA_N, 256));

    printf("rsa-unit: %s (%d failure(s))\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
