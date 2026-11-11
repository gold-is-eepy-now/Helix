/* tls13-server-unit.c — builds ServerHello messages by hand and checks that the
 * parser accepts the good one and refuses each kind of bad one. */
#include <stdio.h>
#include <string.h>

#include "tls13_server.h"

static int failures;

static void check(const char *name, int ok) {
    printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) failures++;
}

/* Builds a ServerHello with the given fields; returns its length. */
static size_t build(uint8_t *m, const uint8_t random[32], const uint8_t sid[32],
                    uint16_t suite, uint16_t version, uint16_t group, int drop_share) {
    uint8_t *p = m + 4;
    *p++ = 0x03; *p++ = 0x03;
    memcpy(p, random, 32); p += 32;
    *p++ = 32; memcpy(p, sid, 32); p += 32;
    *p++ = (uint8_t)(suite >> 8); *p++ = (uint8_t)suite;
    *p++ = 0;
    uint8_t *ext_len = p; p += 2;
    uint8_t *e = p;
    *p++ = 0x00; *p++ = 0x2b; *p++ = 0; *p++ = 2;
    *p++ = (uint8_t)(version >> 8); *p++ = (uint8_t)version;
    if (!drop_share) {
        *p++ = 0x00; *p++ = 0x33; *p++ = 0; *p++ = 36;
        *p++ = (uint8_t)(group >> 8); *p++ = (uint8_t)group;
        *p++ = 0; *p++ = 32;
        for (int i = 0; i < 32; i++) *p++ = (uint8_t)(0x40 + i);
    }
    ext_len[0] = (uint8_t)((p - e) >> 8);
    ext_len[1] = (uint8_t)(p - e);
    size_t body = (size_t)(p - (m + 4));
    m[0] = 0x02;
    m[1] = (uint8_t)(body >> 16); m[2] = (uint8_t)(body >> 8); m[3] = (uint8_t)body;
    return body + 4;
}

int main(void) {
    uint8_t random[32], sid[32], m[256], pub[32];
    memset(random, 0x5a, 32);
    memset(sid, 0x22, 32);

    size_t len = build(m, random, sid, 0x1303, 0x0304, 0x001d, 0);
    memset(pub, 0, 32);
    check("accepts a good ServerHello", tls13_parse_server_hello(m, len, sid, pub) == 1);
    check("returns the server key share", pub[0] == 0x40 && pub[31] == 0x5f);

    uint8_t other_sid[32];
    memset(other_sid, 0x23, 32);
    check("refuses a mismatched session id", !tls13_parse_server_hello(m, len, other_sid, pub));

    len = build(m, random, sid, 0x1301, 0x0304, 0x001d, 0);
    check("refuses a suite we did not offer", !tls13_parse_server_hello(m, len, sid, pub));

    len = build(m, random, sid, 0x1303, 0x0303, 0x001d, 0);
    check("refuses a version other than TLS 1.3", !tls13_parse_server_hello(m, len, sid, pub));

    len = build(m, random, sid, 0x1303, 0x0304, 0x0017, 0);
    check("refuses a key share in another group", !tls13_parse_server_hello(m, len, sid, pub));

    len = build(m, random, sid, 0x1303, 0x0304, 0x001d, 1);
    check("refuses a ServerHello without a key share", !tls13_parse_server_hello(m, len, sid, pub));

    static const uint8_t hrr[32] = {
        0xCF, 0x21, 0xAD, 0x74, 0xE5, 0x9A, 0x61, 0x11, 0xBE, 0x1D, 0x8C, 0x02, 0x1E, 0x65, 0xB8, 0x91,
        0xC2, 0xA2, 0x11, 0x16, 0x7A, 0xBB, 0x8C, 0x5E, 0x07, 0x9E, 0x09, 0xE2, 0xC8, 0xA8, 0x33, 0x9C,
    };
    len = build(m, hrr, sid, 0x1303, 0x0304, 0x001d, 0);
    check("refuses a HelloRetryRequest", !tls13_parse_server_hello(m, len, sid, pub));

    len = build(m, random, sid, 0x1303, 0x0304, 0x001d, 0);
    m[len - 1] ^= 0xff;   /* corrupt the key share bytes: still parses, but shows the key changed */
    check("a changed key share still parses (the key is checked later, by the handshake)",
          tls13_parse_server_hello(m, len, sid, pub) == 1);

    len = build(m, random, sid, 0x1303, 0x0304, 0x001d, 0);
    check("refuses a truncated message", !tls13_parse_server_hello(m, len - 1, sid, pub));

    if (failures) {
        printf("tls13-server-unit: %d failure(s)\n", failures);
        return 1;
    }
    printf("tls13-server-unit: all checks passed\n");
    return 0;
}
