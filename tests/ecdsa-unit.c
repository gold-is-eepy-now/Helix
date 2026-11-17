/* ecdsa-unit.c — checks ecdsa_p256.c against signatures made by the Python `cryptography`
 * library (tests/ecdsa-vectors.h, from tests/gen-ecdsa-vectors.py). Every good signature
 * must verify and every altered input must be refused. */
#include <stdio.h>
#include <string.h>

#include "ecdsa_p256.h"
#include "ecdsa-vectors.h"

static int failures;

static void check(const char *name, int ok) {
    printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) failures++;
}

int main(void) {
    check("ECDSA P-256 signature verifies",
          ecdsa_p256_verify_sha256(ECDSA_PUB, 65, ECDSA_HASH, ECDSA_SIG, ECDSA_SIG_LEN));

    check("refuses a signature for another message",
          !ecdsa_p256_verify_sha256(ECDSA_PUB, 65, ECDSA_HASH, ECDSA_OTHER_SIG, ECDSA_OTHER_SIG_LEN));

    uint8_t bad_hash[32];
    memcpy(bad_hash, ECDSA_HASH, 32);
    bad_hash[31] ^= 1;
    check("refuses a different hash",
          !ecdsa_p256_verify_sha256(ECDSA_PUB, 65, bad_hash, ECDSA_SIG, ECDSA_SIG_LEN));

    uint8_t flipped[ECDSA_SIG_LEN];
    memcpy(flipped, ECDSA_SIG, ECDSA_SIG_LEN);
    flipped[ECDSA_SIG_LEN - 1] ^= 0x01;              /* last byte of s */
    check("refuses a one-bit change in s",
          !ecdsa_p256_verify_sha256(ECDSA_PUB, 65, ECDSA_HASH, flipped, ECDSA_SIG_LEN));

    check("refuses a truncated signature",
          !ecdsa_p256_verify_sha256(ECDSA_PUB, 65, ECDSA_HASH, ECDSA_SIG, ECDSA_SIG_LEN - 1));

    uint8_t trailing[ECDSA_SIG_LEN + 1];
    memcpy(trailing, ECDSA_SIG, ECDSA_SIG_LEN);
    trailing[ECDSA_SIG_LEN] = 0x00;
    check("refuses trailing bytes after the DER sequence",
          !ecdsa_p256_verify_sha256(ECDSA_PUB, 65, ECDSA_HASH, trailing, ECDSA_SIG_LEN + 1));

    /* A key with y changed is not on the curve, so it must be refused before any use. */
    uint8_t bad_pub[65];
    memcpy(bad_pub, ECDSA_PUB, 65);
    bad_pub[64] ^= 0x01;
    check("refuses a public point not on the curve",
          !ecdsa_p256_verify_sha256(bad_pub, 65, ECDSA_HASH, ECDSA_SIG, ECDSA_SIG_LEN));

    check("refuses a key that is not uncompressed",
          !ecdsa_p256_verify_sha256(ECDSA_PUB, 64, ECDSA_HASH, ECDSA_SIG, ECDSA_SIG_LEN));

    printf("ecdsa-unit: %s (%d failure(s))\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
