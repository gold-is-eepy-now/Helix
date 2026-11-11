/* tls13-h-unit.c — checks kernel/src/tls13.c (the second key-schedule API)
 * against the same RFC 8448 section 3 values as tls13-unit.c. */
#include <stdio.h>
#include <string.h>

#include "sha256.h"
#include "tls13.h"

static int failures;

static void from_hex(const char *hex, uint8_t *out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        sscanf(hex + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
}

static void expect(const char *name, const uint8_t *got, size_t n, const char *want_hex) {
    uint8_t want[64];
    from_hex(want_hex, want, n);
    if (memcmp(got, want, n) == 0) {
        printf("PASS: %s\n", name);
    } else {
        printf("FAIL: %s\n", name);
        failures++;
    }
}

int main(void) {
    uint8_t th_sh[32], th_sf[32], shared[32], early[32], hs[32], master[32];
    uint8_t c_hs[32], s_hs[32], c_ap[32], s_ap[32], key[16], iv[12];
    from_hex("860c06edc07858ee8e78f0e7428c58edd6b43f2ca3e6e95f02ed063cf0e1cad8", th_sh, 32);
    from_hex("9608102a0f1ccc6db6250b7b7e417b1a000eaada3daae4777a7686c9ff83df13", th_sf, 32);
    from_hex("8bd4054fb55b9d63fdfbacf9f04b9f0d35e6d63f537563efd46272900f89492d", shared, 32);

    tls13_early_secret(early);
    expect("early secret", early, 32, "33ad0a1c607ec03b09e6cd9893680ce210adf300aa1f2660e1b22e10f170f92a");
    tls13_handshake_secret(hs, early, shared);
    expect("handshake secret", hs, 32, "1dc826e93606aa6fdc0aadc12f741b01046aa6b99f691ed221a9f0ca043fbeac");
    tls13_derive_secret(c_hs, hs, "c hs traffic", th_sh);
    expect("client handshake traffic secret", c_hs, 32, "b3eddb126e067f35a780b3abf45e2d8f3b1a950738f52e9600746a0e27a55a21");
    tls13_derive_secret(s_hs, hs, "s hs traffic", th_sh);
    expect("server handshake traffic secret", s_hs, 32, "b67b7d690cc16c4e75e54213cb2d37b4e9c912bcded9105d42befd59d391ad38");
    tls13_traffic_key_iv(key, iv, c_hs);
    expect("client handshake key", key, 16, "dbfaa693d1762c5b666af5d950258d01");
    expect("client handshake IV", iv, 12, "5bd3c71b836e0b76bb73265f");
    tls13_master_secret(master, hs);
    expect("master secret", master, 32, "18df06843d13a08bf2a449844c5f8a478001bc4d4c627984d5a41da8d0402919");
    tls13_derive_secret(c_ap, master, "c ap traffic", th_sf);
    expect("client application traffic secret 0", c_ap, 32, "9e40646ce79a7f9dc05af8889bce6552875afa0b06df0087f792ebb7c17504a5");
    tls13_derive_secret(s_ap, master, "s ap traffic", th_sf);
    expect("server application traffic secret 0", s_ap, 32, "a11af9f05531f856ad47116b45a950328204b4f44bfb6b3a4b4f1f3fcb631643");
    tls13_traffic_key_iv(key, iv, s_ap);
    expect("server application key", key, 16, "9f02283b6c9c07efc26bb9f2ac92e356");
    expect("server application IV", iv, 12, "cf782b88dd83549aadf1e984");

    if (failures) { printf("tls13-h-unit: %d failure(s)\n", failures); return 1; }
    printf("tls13-h-unit: all checks passed\n");
    return 0;
}
