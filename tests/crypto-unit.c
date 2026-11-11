/* crypto-unit.c — checks the kernel's X25519 and ChaCha20-Poly1305 against the
 * published vectors (RFC 7748 section 5.2 and 6.1, RFC 8439 sections 2.3.2 and
 * 2.8.2). Built and run by `make test`. */
#include <stdio.h>
#include <string.h>

#include "chacha20poly1305.h"
#include "x25519.h"

static int failures;

static void from_hex(const char *hex, uint8_t *out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        sscanf(hex + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
}

static void to_hex(const uint8_t *b, size_t n, char *out) {
    static const char d[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = d[b[i] >> 4];
        out[2 * i + 1] = d[b[i] & 15];
    }
    out[2 * n] = '\0';
}

static void expect_bytes(const char *name, const uint8_t *got, size_t n, const char *want) {
    char text[2 * 512 + 1];
    to_hex(got, n, text);
    if (strcmp(text, want) == 0) {
        printf("PASS: %s\n", name);
    } else {
        printf("FAIL: %s\n  got  %s\n  want %s\n", name, text, want);
        failures++;
    }
}

static void check(const char *name, int ok) {
    printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) failures++;
}

int main(void) {
    /* RFC 7748 6.1: Alice and Bob derive the same shared secret. */
    uint8_t a_priv[32], b_priv[32], a_pub[32], b_pub[32], s1[32], s2[32];
    from_hex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", a_priv, 32);
    from_hex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb", b_priv, 32);
    x25519_base(a_pub, a_priv);
    x25519_base(b_pub, b_priv);
    expect_bytes("x25519 Alice public key", a_pub, 32,
                 "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");
    expect_bytes("x25519 Bob public key", b_pub, 32,
                 "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f");
    x25519(s1, a_priv, b_pub);
    x25519(s2, b_priv, a_pub);
    expect_bytes("x25519 shared secret (Alice side)", s1, 32,
                 "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
    expect_bytes("x25519 shared secret (Bob side)", s2, 32,
                 "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");

    /* RFC 7748 5.2: one iteration of k = u = 9. */
    uint8_t k[32] = {9}, u[32] = {9}, out[32];
    x25519(out, k, u);
    expect_bytes("x25519 RFC 7748 iterate once", out, 32,
                 "422c8e7a6227d7bca1350b3e2bb7279f7897b87bb6854b783c60e80311ae3079");

    /* RFC 8439 2.3.2: ChaCha20 block function, counter 1. */
    uint8_t key[32], nonce[12], ks[16], zeros[16] = {0};
    for (int i = 0; i < 32; i++) key[i] = (uint8_t)i;
    from_hex("000000090000004a00000000", nonce, 12);
    chacha20_xor(ks, zeros, 16, key, nonce, 1);
    expect_bytes("ChaCha20 RFC 8439 2.3.2 keystream", ks, 16, "10f1e7e4d13b5915500fdd1fa32071c4");

    /* RFC 8439 2.8.2: AEAD seal with AAD, checked against ciphertext and tag. */
    static const char pt[] =
        "Ladies and Gentlemen of the class of '99: If I could offer you only one "
        "tip for the future, sunscreen would be it.";
    size_t pt_len = strlen(pt);
    uint8_t aead_key[32], aead_nonce[12], aad[12], ct[sizeof pt], tag[16];
    for (int i = 0; i < 32; i++) aead_key[i] = (uint8_t)(0x80 + i);
    from_hex("070000004041424344454647", aead_nonce, 12);
    from_hex("50515253c0c1c2c3c4c5c6c7", aad, 12);
    aead_chacha20poly1305_seal(ct, tag, aead_key, aead_nonce, aad, sizeof aad,
                               (const uint8_t *)pt, pt_len);
    expect_bytes("AEAD RFC 8439 2.8.2 tag", tag, 16, "1ae10b594f09e26a7e902ecbd0600691");
    expect_bytes("AEAD RFC 8439 2.8.2 ciphertext prefix", ct, 16, "d31a8d34648e60db7b86afbc53ef7ec2");

    /* Round trip, then reject a flipped bit and leave the output untouched. */
    uint8_t back[sizeof pt];
    check("AEAD open accepts the genuine tag",
          aead_chacha20poly1305_open(back, tag, aead_key, aead_nonce, aad, sizeof aad, ct, pt_len)
          && memcmp(back, pt, pt_len) == 0);
    uint8_t bad[sizeof pt];
    memcpy(bad, ct, pt_len);
    bad[5] ^= 1;
    memset(back, 0xAA, sizeof back);
    check("AEAD open rejects a modified ciphertext",
          !aead_chacha20poly1305_open(back, tag, aead_key, aead_nonce, aad, sizeof aad, bad, pt_len)
          && back[0] == 0xAA);
    check("AEAD open rejects a modified AAD",
          !aead_chacha20poly1305_open(back, tag, aead_key, aead_nonce, aad + 1, sizeof aad - 1, ct, pt_len));

    if (failures) {
        printf("crypto-unit: %d failure(s)\n", failures);
        return 1;
    }
    printf("crypto-unit: all checks passed\n");
    return 0;
}
