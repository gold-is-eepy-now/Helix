/* sha256-unit.c — checks the kernel's SHA-256, HMAC and PBKDF2 against the
 * published test vectors (FIPS 180-4, RFC 4231 case 1, RFC 6070-style PBKDF2
 * vectors for HMAC-SHA256). Built and run by `make test`. */
#include <stdio.h>
#include <string.h>

#include "sha256.h"

static int failures;

static void hex(const uint8_t *b, size_t n, char *out) {
    static const char d[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = d[b[i] >> 4];
        out[2 * i + 1] = d[b[i] & 15];
    }
    out[2 * n] = '\0';
}

static void expect(const char *name, const uint8_t *got, size_t n, const char *want) {
    char text[2 * 64 + 1];
    hex(got, n, text);
    if (strcmp(text, want) == 0) {
        printf("PASS: %s\n", name);
    } else {
        printf("FAIL: %s\n  got  %s\n  want %s\n", name, text, want);
        failures++;
    }
}

int main(void) {
    uint8_t d[SHA256_DIGEST_LEN];

    sha256("abc", 3, d);
    expect("sha256(\"abc\")", d, sizeof d,
           "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    sha256("", 0, d);
    expect("sha256(\"\")", d, sizeof d,
           "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    const char *two_block = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha256(two_block, strlen(two_block), d);
    expect("sha256 two-block message", d, sizeof d,
           "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    /* Feed the same message in uneven pieces to exercise the buffering. */
    sha256_ctx_t c;
    sha256_init(&c);
    sha256_update(&c, two_block, 5);
    sha256_update(&c, two_block + 5, 40);
    sha256_update(&c, two_block + 45, strlen(two_block) - 45);
    sha256_final(&c, d);
    expect("sha256 incremental update", d, sizeof d,
           "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    uint8_t key[20];
    memset(key, 0x0b, sizeof key);
    hmac_sha256(key, sizeof key, (const uint8_t *)"Hi There", 8, d);
    expect("HMAC-SHA256 RFC 4231 case 1", d, sizeof d,
           "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");

    pbkdf2_sha256((const uint8_t *)"password", 8, (const uint8_t *)"salt", 4, 1, d, sizeof d);
    expect("PBKDF2-HMAC-SHA256 c=1", d, sizeof d,
           "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");

    pbkdf2_sha256((const uint8_t *)"password", 8, (const uint8_t *)"salt", 4, 4096, d, sizeof d);
    expect("PBKDF2-HMAC-SHA256 c=4096", d, sizeof d,
           "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a");

    if (failures) {
        printf("sha256-unit: %d failure(s)\n", failures);
        return 1;
    }
    printf("sha256-unit: all checks passed\n");
    return 0;
}
