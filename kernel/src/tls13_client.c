/* tls13_client.c — see tls13_client.h. */
#include "tls13_client.h"

#include <stddef.h>
#include <stdint.h>

#include "hkdf.h"
#include "kstring.h"
#include "sha256.h"
#include "tls13.h"
#include "tls13_hello.h"
#include "tls13_record.h"
#include "tls13_server.h"
#include "x25519.h"

#define HS_SERVER_HELLO        2u
#define HS_ENCRYPTED_EXT       8u
#define HS_CERTIFICATE         11u
#define HS_CERT_VERIFY         15u
#define HS_FINISHED            20u

static size_t fail(tls13_client_t *c) {
    c->state = TLS_ST_FAILED;
    return (size_t)-1;
}

/* Hash of the transcript so far, without disturbing the running transcript. */
static void snapshot(const sha256_ctx_t *t, uint8_t out[32]) {
    sha256_ctx_t tmp = *t;
    sha256_final(&tmp, out);
}

static void cipher_from_secret(tls13_cipher_t *cs, const uint8_t secret[32]) {
    tls13_expand_label(cs->key, 32, secret, "key", NULL, 0);
    tls13_expand_label(cs->iv, 12, secret, "iv", NULL, 0);
    cs->seq = 0;
}

/* Compares without an early exit on the first differing byte. */
static int bytes_equal(const uint8_t *a, const uint8_t *b, size_t n) {
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}

/* Derives the handshake secrets from the ServerHello. Called once the transcript
 * ends with the ServerHello. */
static int derive_handshake(tls13_client_t *c) {
    uint8_t shared[32], early[32], th_sh[32];
    x25519(shared, c->priv, c->server_pub);
    /* An all-zero shared secret means the server sent a low-order point. */
    uint8_t acc = 0;
    for (size_t i = 0; i < 32; i++) acc |= shared[i];
    if (acc == 0) return 0;

    tls13_early_secret(early);
    tls13_handshake_secret(c->hs_secret, early, shared);
    snapshot(&c->transcript, th_sh);
    tls13_derive_secret(c->c_hs_secret, c->hs_secret, "c hs traffic", th_sh);
    tls13_derive_secret(c->s_hs_secret, c->hs_secret, "s hs traffic", th_sh);
    cipher_from_secret(&c->hs_rx, c->s_hs_secret);
    cipher_from_secret(&c->hs_tx, c->c_hs_secret);
    c->state = TLS_ST_WAIT_EE;
    return 1;
}

/* The server's Finished: verify it, send ours, and derive the application keys. */
static int on_server_finished(tls13_client_t *c, const uint8_t *m, size_t n) {
    if (n != 36 || m[0] != HS_FINISHED) return 0;

    uint8_t th_cv[32], key[32], want[32];
    snapshot(&c->transcript, th_cv);
    tls13_expand_label(key, 32, c->s_hs_secret, "finished", NULL, 0);
    hmac_sha256(key, 32, th_cv, 32, want);
    if (!bytes_equal(want, m + 4, 32)) return 0;

    sha256_update(&c->transcript, m, n);
    uint8_t th_sf[32];
    snapshot(&c->transcript, th_sf);

    /* Client Finished: verify_data = HMAC(client finished key, transcript through server Finished). */
    uint8_t fin[36] = { HS_FINISHED, 0, 0, 32 };
    tls13_expand_label(key, 32, c->c_hs_secret, "finished", NULL, 0);
    hmac_sha256(key, 32, th_sf, 32, fin + 4);
    c->pending_len = tls13_seal(c->pending, &c->hs_tx, TLS_CT_HANDSHAKE, fin, sizeof fin);

    uint8_t master[32], c_ap[32], s_ap[32];
    tls13_master_secret(master, c->hs_secret);
    tls13_derive_secret(c_ap, master, "c ap traffic", th_sf);
    tls13_derive_secret(s_ap, master, "s ap traffic", th_sf);
    cipher_from_secret(&c->app_tx, c_ap);
    cipher_from_secret(&c->app_rx, s_ap);

    c->state = TLS_ST_WAIT_AUTH;
    return 1;
}

/* One complete handshake message (header included). The transcript takes every
 * message except the client Finished, which is added to nothing. */
static int on_message(tls13_client_t *c, const uint8_t *m, size_t n) {
    uint8_t type = m[0];
    switch (c->state) {
    case TLS_ST_WAIT_SH:
        if (type != HS_SERVER_HELLO) return 0;
        if (!tls13_parse_server_hello(m, n, c->session_id, c->server_pub)) return 0;
        sha256_update(&c->transcript, m, n);
        return derive_handshake(c);
    case TLS_ST_WAIT_EE:
        if (type != HS_ENCRYPTED_EXT) return 0;
        sha256_update(&c->transcript, m, n);
        c->state = TLS_ST_WAIT_CERT;
        return 1;
    case TLS_ST_WAIT_CERT:
        /* Not verified yet; see tls13_client.h. */
        if (type != HS_CERTIFICATE) return 0;
        sha256_update(&c->transcript, m, n);
        c->state = TLS_ST_WAIT_CV;
        return 1;
    case TLS_ST_WAIT_CV:
        /* Signature not verified yet; see tls13_client.h. */
        if (type != HS_CERT_VERIFY) return 0;
        sha256_update(&c->transcript, m, n);
        c->state = TLS_ST_WAIT_FIN;
        return 1;
    case TLS_ST_WAIT_FIN:
        return on_server_finished(c, m, n);
    default:
        return 0;
    }
}

/* Runs every complete handshake message in hs_buf, keeping any partial one. */
static int process_messages(tls13_client_t *c) {
    while (c->hs_len >= 4) {
        size_t mlen = ((size_t)c->hs_buf[1] << 16) | ((size_t)c->hs_buf[2] << 8) | c->hs_buf[3];
        if (4 + mlen > TLS_HS_BUF_MAX) return 0;
        if (c->hs_len < 4 + mlen) break;
        if (!on_message(c, c->hs_buf, 4 + mlen)) return 0;
        memmove(c->hs_buf, c->hs_buf + 4 + mlen, c->hs_len - 4 - mlen);
        c->hs_len -= 4 + mlen;
    }
    return 1;
}

static int append_handshake(tls13_client_t *c, const uint8_t *data, size_t n) {
    if (c->hs_len + n > TLS_HS_BUF_MAX) return 0;
    memcpy(c->hs_buf + c->hs_len, data, n);
    c->hs_len += n;
    return process_messages(c);
}

/* One whole record. The ServerHello is the only plaintext handshake record; everything
 * after it is encrypted under the handshake keys. */
static int handle_record(tls13_client_t *c, const uint8_t *rec, size_t rec_len) {
    if (rec[0] == TLS_CT_CHANGE_CIPHER) return 1;   /* compatibility CCS, ignored */
    if (c->state == TLS_ST_WAIT_SH) {
        if (rec[0] != TLS_CT_HANDSHAKE) return 0;
        return append_handshake(c, rec + TLS_RECORD_HDR_LEN, rec_len - TLS_RECORD_HDR_LEN);
    }
    if (rec[0] != TLS_CT_APPDATA) return 0;
    size_t body = rec_len - TLS_RECORD_HDR_LEN;
    if (c->hs_len + body > TLS_HS_BUF_MAX) return 0;

    /* Decrypt straight into the handshake buffer; tls13_open writes nothing on failure. */
    size_t plain_len = 0;
    uint8_t ct = 0;
    if (!tls13_open(c->hs_buf + c->hs_len, &plain_len, &ct, &c->hs_rx, rec, rec_len)) return 0;
    if (ct != TLS_CT_HANDSHAKE) return 0;
    c->hs_len += plain_len;
    return process_messages(c);
}

void tls13_client_init(tls13_client_t *c, const uint8_t random[32],
                       const uint8_t session_id[32], const uint8_t priv[32]) {
    memset(c, 0, sizeof *c);
    memcpy(c->random, random, 32);
    memcpy(c->session_id, session_id, 32);
    memcpy(c->priv, priv, 32);
    x25519_base(c->pub, c->priv);
    sha256_init(&c->transcript);
    c->state = TLS_ST_IDLE;
}

size_t tls13_client_hello(tls13_client_t *c, uint8_t *out, size_t cap, const char *host) {
    if (c->state != TLS_ST_IDLE) return 0;
    uint8_t msg[512];
    size_t n = tls13_build_client_hello(msg, sizeof msg, c->random, c->session_id, c->pub, host);
    if (n == 0 || cap < TLS_RECORD_HDR_LEN + n) return 0;
    /* Legacy record version 0x0301, as TLS 1.3 clients send in the first record. */
    out[0] = TLS_CT_HANDSHAKE;
    out[1] = 0x03;
    out[2] = 0x01;
    out[3] = (uint8_t)(n >> 8);
    out[4] = (uint8_t)n;
    memcpy(out + TLS_RECORD_HDR_LEN, msg, n);
    sha256_update(&c->transcript, msg, n);
    c->state = TLS_ST_WAIT_SH;
    return TLS_RECORD_HDR_LEN + n;
}

size_t tls13_client_feed(tls13_client_t *c, const uint8_t *in, size_t len) {
    if (c->state == TLS_ST_IDLE || c->state == TLS_ST_FAILED) return fail(c);
    size_t used = 0;
    while (len - used >= TLS_RECORD_HDR_LEN) {
        const uint8_t *rec = in + used;
        size_t body = ((size_t)rec[3] << 8) | rec[4];
        if (body > TLS_MAX_PLAIN + 256u) return fail(c);
        if (len - used < TLS_RECORD_HDR_LEN + body) break;   /* wait for the rest of it */
        if (!handle_record(c, rec, TLS_RECORD_HDR_LEN + body)) return fail(c);
        used += TLS_RECORD_HDR_LEN + body;
    }
    return used;
}

size_t tls13_client_take_output(tls13_client_t *c, uint8_t *out, size_t cap) {
    if (c->pending_len == 0 || cap < c->pending_len) return 0;
    size_t n = c->pending_len;
    memcpy(out, c->pending, n);
    c->pending_len = 0;
    return n;
}

int tls13_client_authenticate(tls13_client_t *c) {
    if (c->state != TLS_ST_WAIT_AUTH) return 0;
    c->state = TLS_ST_READY;
    return 1;
}

int tls13_client_app_keys(const tls13_client_t *c, tls13_cipher_t *tx, tls13_cipher_t *rx) {
    if (c->state != TLS_ST_READY) return 0;
    *tx = c->app_tx;
    *rx = c->app_rx;
    return 1;
}
