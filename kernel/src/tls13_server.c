/* tls13_server.c — see tls13_server.h. */
#include "tls13_server.h"

#include <stddef.h>
#include <stdint.h>

#include "kstring.h"

/* Fixed random value that marks a HelloRetryRequest (RFC 8446 section 4.1.3). */
static const uint8_t hello_retry_random[32] = {
    0xCF, 0x21, 0xAD, 0x74, 0xE5, 0x9A, 0x61, 0x11, 0xBE, 0x1D, 0x8C, 0x02, 0x1E, 0x65, 0xB8, 0x91,
    0xC2, 0xA2, 0x11, 0x16, 0x7A, 0xBB, 0x8C, 0x5E, 0x07, 0x9E, 0x09, 0xE2, 0xC8, 0xA8, 0x33, 0x9C,
};

static uint32_t be16(const uint8_t *p) { return ((uint32_t)p[0] << 8) | p[1]; }

int tls13_parse_server_hello(const uint8_t *msg, size_t len,
                             const uint8_t session_id[32],
                             uint8_t server_pub[32]) {
    /* Header (4), version (2), random (32), session id length (1) + id (32),
     * suite (2), compression (1), extensions length (2). */
    if (len < 4 + 2 + 32 + 1 + 32 + 2 + 1 + 2) return 0;
    if (msg[0] != 0x02) return 0;                            /* server_hello */
    size_t body = ((size_t)msg[1] << 16) | ((size_t)msg[2] << 8) | msg[3];
    if (body + 4 != len) return 0;

    const uint8_t *p = msg + 4;
    if (be16(p) != 0x0303) return 0;                          /* legacy_version */
    p += 2;
    if (memcmp(p, hello_retry_random, 32) == 0) return 0;     /* HelloRetryRequest */
    p += 32;
    if (p[0] != 32 || memcmp(p + 1, session_id, 32) != 0) return 0;   /* echoed id */
    p += 33;
    if (be16(p) != 0x1303) return 0;                          /* our one suite */
    p += 2;
    if (p[0] != 0) return 0;                                  /* compression */
    p += 1;

    size_t ext_len = be16(p);
    p += 2;
    if ((size_t)(p - msg) + ext_len != len) return 0;

    const uint8_t *e = p;
    const uint8_t *end = p + ext_len;
    int saw_version = 0, saw_share = 0;
    while (e + 4 <= end) {
        uint32_t type = be16(e);
        uint32_t n = be16(e + 2);
        const uint8_t *d = e + 4;
        if (d + n > end) return 0;
        if (type == 0x002b) {                                 /* supported_versions */
            if (n != 2 || be16(d) != 0x0304) return 0;
            saw_version = 1;
        } else if (type == 0x0033) {                          /* key_share */
            if (n != 36 || be16(d) != 0x001d || be16(d + 2) != 32) return 0;
            memcpy(server_pub, d + 4, 32);
            saw_share = 1;
        }
        e = d + n;
    }
    return e == end && saw_version && saw_share;
}
