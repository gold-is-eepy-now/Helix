/* tls13-client-unit.c — drives the TLS 1.3 client state machine against a server
 * built from the same primitives. Checks: the flight is parsed, the client's Finished
 * verifies, application keys are withheld until authentication, both directions of
 * application traffic work afterwards, and a server Finished with a wrong verify_data
 * is rejected. The server side is ours, so this checks self-consistency and the
 * message flow, not interop with a real server (that needs a live HTTPS test). */
#include <stdio.h>
#include <string.h>

#include "hkdf.h"
#include "sha256.h"
#include "tls13.h"
#include "tls13_client.h"
#include "tls13_record.h"
#include "x25519.h"

static int failures;

static void check(const char *name, int ok) {
    printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) failures++;
}

static void cipher_from(tls13_cipher_t *cs, const uint8_t secret[32]) {
    tls13_expand_label(cs->key, 32, secret, "key", NULL, 0);
    tls13_expand_label(cs->iv, 12, secret, "iv", NULL, 0);
    cs->seq = 0;
}

static void snap(const sha256_ctx_t *t, uint8_t out[32]) {
    sha256_ctx_t tmp = *t;
    sha256_final(&tmp, out);
}

/* A ServerHello echoing sid and offering our x25519 key share. */
static size_t build_server_hello(uint8_t *m, const uint8_t sid[32], const uint8_t pub[32]) {
    uint8_t *p = m + 4;
    *p++ = 3; *p++ = 3;
    memset(p, 0x5a, 32); p += 32;
    *p++ = 32; memcpy(p, sid, 32); p += 32;
    *p++ = 0x13; *p++ = 0x03;
    *p++ = 0;
    uint8_t *elen = p; p += 2;
    uint8_t *e0 = p;
    *p++ = 0; *p++ = 0x2b; *p++ = 0; *p++ = 2; *p++ = 3; *p++ = 4;
    *p++ = 0; *p++ = 0x33; *p++ = 0; *p++ = 36; *p++ = 0; *p++ = 0x1d; *p++ = 0; *p++ = 32;
    memcpy(p, pub, 32); p += 32;
    elen[0] = (uint8_t)((p - e0) >> 8);
    elen[1] = (uint8_t)(p - e0);
    size_t body = (size_t)(p - (m + 4));
    m[0] = 2;
    m[1] = (uint8_t)(body >> 16); m[2] = (uint8_t)(body >> 8); m[3] = (uint8_t)body;
    return body + 4;
}

/* Runs one handshake. With tamper set, the server's Finished has a wrong verify_data
 * and the client must refuse it. */
static void run(int tamper) {
    uint8_t cli_random[32], sid[32], cli_priv[32], srv_priv[32], srv_pub[32], cli_pub[32];
    memset(cli_random, 0x11, 32);
    memset(sid, 0x22, 32);
    for (int i = 0; i < 32; i++) {
        cli_priv[i] = (uint8_t)(i * 3 + 1);
        srv_priv[i] = (uint8_t)(i * 5 + 7);
    }
    x25519_base(srv_pub, srv_priv);
    x25519_base(cli_pub, cli_priv);

    tls13_client_t c;
    tls13_client_init(&c, cli_random, sid, cli_priv);
    uint8_t ch[600];
    size_t chn = tls13_client_hello(&c, ch, sizeof ch, "example.com");
    check(tamper ? "tamper run: ClientHello built" : "ClientHello built", chn > 5 && ch[0] == 22);

    /* Server: transcript = ClientHello, ServerHello, then its flight. */
    sha256_ctx_t t;
    sha256_init(&t);
    sha256_update(&t, ch + 5, chn - 5);
    uint8_t sh[128];
    size_t shn = build_server_hello(sh, sid, srv_pub);
    sha256_update(&t, sh, shn);

    uint8_t th_sh[32], shared[32], early[32], hs[32], c_hs[32], s_hs[32];
    snap(&t, th_sh);
    x25519(shared, srv_priv, cli_pub);
    tls13_early_secret(early);
    tls13_handshake_secret(hs, early, shared);
    tls13_derive_secret(c_hs, hs, "c hs traffic", th_sh);
    tls13_derive_secret(s_hs, hs, "s hs traffic", th_sh);

    /* EncryptedExtensions, Certificate (dummy, not verified yet), CertificateVerify
     * (dummy), Finished. They go in one encrypted record. */
    static const uint8_t ee[6] = { 8, 0, 0, 2, 0, 0 };
    static const uint8_t cert[17] = { 11, 0, 0, 13, 0, 0, 0, 9, 0, 0, 4, 'd', 'u', 'm', 'm', 0, 0 };
    static const uint8_t cv[12] = { 15, 0, 0, 8, 8, 4, 0, 4, 'd', 'u', 'm', 'm' };
    uint8_t flight[128];
    size_t fl = 0;
    memcpy(flight + fl, ee, sizeof ee); fl += sizeof ee;
    memcpy(flight + fl, cert, sizeof cert); fl += sizeof cert;
    memcpy(flight + fl, cv, sizeof cv); fl += sizeof cv;
    sha256_update(&t, ee, sizeof ee);
    sha256_update(&t, cert, sizeof cert);
    sha256_update(&t, cv, sizeof cv);

    uint8_t th_cv[32], fk[32], fin[32];
    snap(&t, th_cv);
    tls13_expand_label(fk, 32, s_hs, "finished", NULL, 0);
    hmac_sha256(fk, 32, th_cv, 32, fin);
    if (tamper) fin[0] ^= 1;
    uint8_t finmsg[36] = { 20, 0, 0, 32 };
    memcpy(finmsg + 4, fin, 32);
    memcpy(flight + fl, finmsg, sizeof finmsg); fl += sizeof finmsg;
    sha256_update(&t, finmsg, sizeof finmsg);
    uint8_t th_sf[32];
    snap(&t, th_sf);

    /* Wire: plaintext ServerHello record, then the encrypted flight. */
    uint8_t in[700];
    size_t inl = 0;
    in[0] = 22; in[1] = 3; in[2] = 3; in[3] = (uint8_t)(shn >> 8); in[4] = (uint8_t)shn;
    memcpy(in + 5, sh, shn);
    inl = 5 + shn;
    tls13_cipher_t s_tx;
    cipher_from(&s_tx, s_hs);
    inl += tls13_seal(in + inl, &s_tx, TLS_CT_HANDSHAKE, flight, fl);

    /* Feed the first half, then the rest, to exercise partial records. */
    size_t half = 5 + shn + 7;
    size_t used1 = tls13_client_feed(&c, in, half);
    check(tamper ? "tamper run: partial input is held, not rejected" : "partial input is held, not rejected",
          used1 == 5 + shn && c.state == TLS_ST_WAIT_EE);
    size_t used2 = tls13_client_feed(&c, in + used1, inl - used1);

    if (tamper) {
        check("a server Finished with bad verify_data is refused",
              used2 == (size_t)-1 && c.state == TLS_ST_FAILED);
        return;
    }

    check("the whole server flight is consumed", used2 == inl - used1);
    check("client waits for certificate verification", c.state == TLS_ST_WAIT_AUTH);

    uint8_t out[128];
    size_t on = tls13_client_take_output(&c, out, sizeof out);
    check("client Finished is produced as an encrypted record", on > 0 && out[0] == TLS_CT_APPDATA);

    /* The server opens the client Finished under the client handshake key. */
    tls13_cipher_t s_rx;
    cipher_from(&s_rx, c_hs);
    uint8_t pl[64];
    size_t pll = 0;
    uint8_t ct = 0;
    int opened = tls13_open(pl, &pll, &ct, &s_rx, out, on);
    check("server opens the client Finished", opened && ct == TLS_CT_HANDSHAKE && pll == 36 && pl[0] == 20);
    uint8_t cfk[32], cwant[32];
    tls13_expand_label(cfk, 32, c_hs, "finished", NULL, 0);
    hmac_sha256(cfk, 32, th_sf, 32, cwant);
    check("client Finished verify_data is correct", pll == 36 && memcmp(pl + 4, cwant, 32) == 0);

    tls13_cipher_t cap_tx, cap_rx;
    check("application keys are withheld before authentication",
          tls13_client_app_keys(&c, &cap_tx, &cap_rx) == 0);
    check("authenticate moves the client to READY", tls13_client_authenticate(&c) == 1 && c.state == TLS_ST_READY);
    check("application keys are released after authentication",
          tls13_client_app_keys(&c, &cap_tx, &cap_rx) == 1);

    uint8_t master[32], c_ap[32], s_ap[32];
    tls13_master_secret(master, hs);
    tls13_derive_secret(c_ap, master, "c ap traffic", th_sf);
    tls13_derive_secret(s_ap, master, "s ap traffic", th_sf);
    tls13_cipher_t srv_app_rx, srv_app_tx;
    cipher_from(&srv_app_rx, c_ap);
    cipher_from(&srv_app_tx, s_ap);

    const uint8_t req[] = "GET / HTTP/1.1";
    uint8_t wire[64], got[64];
    size_t gl = 0, wl = 0;
    uint8_t gct = 0;
    wl = tls13_seal(wire, &cap_tx, TLS_CT_APPDATA, req, sizeof req - 1);
    check("client request is readable by the server",
          tls13_open(got, &gl, &gct, &srv_app_rx, wire, wl) && gl == sizeof req - 1
          && memcmp(got, req, gl) == 0 && gct == TLS_CT_APPDATA);
    wl = tls13_seal(wire, &srv_app_tx, TLS_CT_APPDATA, req, sizeof req - 1);
    check("server reply is readable by the client",
          tls13_open(got, &gl, &gct, &cap_rx, wire, wl) && gl == sizeof req - 1
          && memcmp(got, req, gl) == 0);
}

int main(void) {
    run(0);
    run(1);
    if (failures) {
        printf("tls13-client-unit: %d failure(s)\n", failures);
        return 1;
    }
    printf("tls13-client-unit: all checks passed\n");
    return 0;
}
