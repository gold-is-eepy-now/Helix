/* hkdf-unit.c — checks HKDF-SHA256 against RFC 5869 test cases 1 and 3. */
#include <stdio.h>
#include <string.h>

#include "hkdf.h"

static int failures;

static void to_hex(const uint8_t *b, size_t n, char *out) {
    static const char d[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = d[b[i] >> 4];
        out[2 * i + 1] = d[b[i] & 15];
    }
    out[2 * n] = '\0';
}

static void expect(const char *name, const uint8_t *got, size_t n, const char *want) {
    char text[2 * 128 + 1];
    to_hex(got, n, text);
    if (strcmp(text, want) == 0) {
        printf("PASS: %s\n", name);
    } else {
        printf("FAIL: %s\n  got  %s\n  want %s\n", name, text, want);
        failures++;
    }
}

int main(void) {
    uint8_t ikm[22], salt[13], info[10], prk[32], okm[42];
    memset(ikm, 0x0b, sizeof ikm);
    for (int i = 0; i < 13; i++) salt[i] = (uint8_t)i;
    for (int i = 0; i < 10; i++) info[i] = (uint8_t)(0xf0 + i);

    /* RFC 5869 A.1 */
    hkdf_extract(prk, salt, sizeof salt, ikm, sizeof ikm);
    expect("HKDF case 1 PRK", prk, 32,
           "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5");
    hkdf_expand(okm, sizeof okm, prk, info, sizeof info);
    expect("HKDF case 1 OKM", okm, 42,
           "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865");

    /* RFC 5869 A.3: empty salt and info. */
    hkdf_extract(prk, NULL, 0, ikm, sizeof ikm);
    expect("HKDF case 3 PRK", prk, 32,
           "19ef24a32c717b167f33a91d6f648bdf96596776afdb6377ac434c1c293ccb04");
    hkdf_expand(okm, sizeof okm, prk, NULL, 0);
    expect("HKDF case 3 OKM", okm, 42,
           "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d9d201395faa4b61a96c8");

    if (failures) {
        printf("hkdf-unit: %d failure(s)\n", failures);
        return 1;
    }
    printf("hkdf-unit: all checks passed\n");
    return 0;
}
