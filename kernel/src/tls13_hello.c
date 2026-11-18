/* tls13_hello.c — see tls13_hello.h. */
#include "tls13_hello.h"

#include <stddef.h>
#include <stdint.h>

#include "kstring.h"

/* A bounds-checked big-endian writer: once it overflows, every later write is dropped
 * and ok stays false. */
typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t len;
    int ok;
} writer_t;

static void put(writer_t *w, const void *data, size_t n) {
    if (!w->ok || w->len + n > w->cap) { w->ok = 0; return; }
    memcpy(w->buf + w->len, data, n);
    w->len += n;
}

static void put8(writer_t *w, uint8_t v) { put(w, &v, 1); }

static void put16(writer_t *w, uint16_t v) {
    uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)v };
    put(w, b, 2);
}

static void put24(writer_t *w, uint32_t v) {
    uint8_t b[3] = { (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    put(w, b, 3);
}

/* Patch a length field that was reserved earlier at position pos. */
static void patch16(writer_t *w, size_t pos, size_t value) {
    if (!w->ok) return;
    w->buf[pos] = (uint8_t)(value >> 8);
    w->buf[pos + 1] = (uint8_t)value;
}

static void patch24(writer_t *w, size_t pos, size_t value) {
    if (!w->ok) return;
    w->buf[pos] = (uint8_t)(value >> 16);
    w->buf[pos + 1] = (uint8_t)(value >> 8);
    w->buf[pos + 2] = (uint8_t)value;
}

size_t tls13_build_client_hello(uint8_t *out, size_t cap,
                                const uint8_t random[32],
                                const uint8_t session_id[32],
                                const uint8_t x25519_pub[32],
                                const char *server_name) {
    writer_t w = { out, cap, 0, 1 };

    put8(&w, 0x01);                       /* HandshakeType client_hello */
    size_t body_len_pos = w.len;
    put24(&w, 0);                         /* patched below */

    put16(&w, 0x0303);                    /* legacy_version */
    put(&w, random, 32);
    put8(&w, 32);                         /* legacy_session_id: echoed by the server */
    put(&w, session_id, 32);

    put16(&w, 2);                         /* cipher_suites: one suite */
    put16(&w, 0x1303);                    /* TLS_CHACHA20_POLY1305_SHA256 */
    put8(&w, 1);                          /* legacy_compression_methods */
    put8(&w, 0);

    size_t ext_len_pos = w.len;
    put16(&w, 0);                         /* extensions length, patched below */
    size_t ext_start = w.len;

    if (server_name && server_name[0]) {  /* server_name (SNI), RFC 6066 */
        size_t n = strlen(server_name);
        put16(&w, 0x0000);
        put16(&w, (uint16_t)(n + 5));
        put16(&w, (uint16_t)(n + 3));
        put8(&w, 0);                      /* name_type host_name */
        put16(&w, (uint16_t)n);
        put(&w, server_name, n);
    }

    put16(&w, 0x000a);                    /* supported_groups: x25519 */
    put16(&w, 4);
    put16(&w, 2);
    put16(&w, 0x001d);

    put16(&w, 0x000d);                    /* signature_algorithms */
    put16(&w, 8);
    put16(&w, 6);
    put16(&w, 0x0403);                    /* ecdsa_secp256r1_sha256 */
    put16(&w, 0x0804);                    /* rsa_pss_rsae_sha256 */
    put16(&w, 0x0401);                    /* rsa_pkcs1_sha256 */

    put16(&w, 0x002b);                    /* supported_versions: TLS 1.3 only */
    put16(&w, 3);
    put8(&w, 2);
    put16(&w, 0x0304);

    put16(&w, 0x0033);                    /* key_share: one x25519 share */
    put16(&w, 38);
    put16(&w, 36);
    put16(&w, 0x001d);
    put16(&w, 32);
    put(&w, x25519_pub, 32);

    patch16(&w, ext_len_pos, w.len - ext_start);
    patch24(&w, body_len_pos, w.len - body_len_pos - 3);
    return w.ok ? w.len : 0;
}
