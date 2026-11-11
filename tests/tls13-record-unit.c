/* tls13-record-unit.c — checks TLS 1.3 record protection (nonce derivation,
 * seal/open round trips, padding, tamper rejection, sequence numbers) and the
 * ClientHello layout, by parsing the bytes back. Built and run by make test. */
#include <stdio.h>
#include <string.h>

#include "tls13_hello.h"
#include "tls13_record.h"

static int failures;

static void check(const char *name, int ok) {
    printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) failures++;
}

static uint32_t be16(const uint8_t *p) { return ((uint32_t)p[0] << 8) | p[1]; }
static uint32_t be24(const uint8_t *p) { return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]; }

int main(void) {
    /* Nonce: the iv XORed with the big-endian sequence number in the last 8 bytes. */
    uint8_t iv[12], n[12];
    memset(iv, 0xAA, sizeof iv);
    tls13_nonce(n, iv, 0);
    check("nonce for sequence 0 is the iv", memcmp(n, iv, 12) == 0);
    tls13_nonce(n, iv, 0x0102030405060708ull);
    check("nonce XORs the sequence number into the last 8 bytes",
          n[0] == 0xAA && n[3] == 0xAA && n[4] == (0xAA ^ 0x01) && n[11] == (0xAA ^ 0x08));

    /* Round trip. Both directions share the key and iv in this test. */
    tls13_cipher_t tx = {0}, rx = {0};
    for (int i = 0; i < 32; i++) tx.key[i] = (uint8_t)(i * 7 + 1);
    memcpy(rx.key, tx.key, 32);
    memset(tx.iv, 0x3C, 12);
    memcpy(rx.iv, tx.iv, 12);

    const uint8_t msg[] = "hello over TLS";
    uint8_t rec[5 + sizeof msg + 1 + 16];
    size_t rec_len = tls13_seal(rec, &tx, TLS_CT_HANDSHAKE, msg, sizeof msg - 1);
    check("record length is header + plain + type byte + tag", rec_len == 5 + (sizeof msg - 1) + 1 + 16);
    check("outer record type is application_data", rec[0] == TLS_CT_APPDATA && rec[1] == 3 && rec[2] == 3);
    check("record header length field matches the body", be16(rec + 3) == rec_len - 5);
    check("sender sequence advances", tx.seq == 1);

    uint8_t out[sizeof rec];
    size_t out_len = 0;
    uint8_t ct = 0;
    check("receiver opens the record", tls13_open(out, &out_len, &ct, &rx, rec, rec_len));
    check("plaintext and inner content type survive", out_len == sizeof msg - 1
          && memcmp(out, msg, out_len) == 0 && ct == TLS_CT_HANDSHAKE);
    check("receiver sequence advances", rx.seq == 1);

    /* Replaying the same record must fail: the sequence number has moved on. */
    check("replayed record is rejected", !tls13_open(out, &out_len, &ct, &rx, rec, rec_len));

    /* Trailing zero bytes inside the plaintext are data, not padding, when the
     * content type follows them. Here the content type is the last byte, so
     * a plaintext of "ab\0\0" must keep all four bytes. */
    tls13_cipher_t tx2 = tx, rx2 = rx;
    tx2.seq = rx2.seq = 0;
    const uint8_t zpad[] = { 'a', 'b', 0, 0 };
    uint8_t rec2[5 + sizeof zpad + 1 + 16];
    size_t rec2_len = tls13_seal(rec2, &tx2, TLS_CT_APPDATA, zpad, sizeof zpad);
    check("zero bytes before the content type are kept",
          tls13_open(out, &out_len, &ct, &rx2, rec2, rec2_len)
          && out_len == 4 && memcmp(out, zpad, 4) == 0 && ct == TLS_CT_APPDATA);

    /* A flipped ciphertext bit fails the tag and leaves the receiver's state alone. */
    tls13_cipher_t tx3 = {0}, rx3 = {0};
    memcpy(tx3.key, tx.key, 32);
    memcpy(rx3.key, tx.key, 32);
    memcpy(tx3.iv, tx.iv, 12);
    memcpy(rx3.iv, tx.iv, 12);
    uint8_t rec3[5 + sizeof msg + 1 + 16];
    size_t rec3_len = tls13_seal(rec3, &tx3, TLS_CT_HANDSHAKE, msg, sizeof msg - 1);
    rec3[7] ^= 1;
    check("modified record is rejected", !tls13_open(out, &out_len, &ct, &rx3, rec3, rec3_len));
    check("failed open leaves the sequence number", rx3.seq == 0);

    /* ClientHello: parse the bytes back and check every field we promised. */
    uint8_t random[32], sid[32], pub[32];
    memset(random, 0x11, 32);
    memset(sid, 0x22, 32);
    memset(pub, 0x33, 32);
    uint8_t hello[512];
    size_t hl = tls13_build_client_hello(hello, sizeof hello, random, sid, pub, "example.com");
    check("ClientHello builds", hl > 0);
    check("handshake type is client_hello", hl > 4 && hello[0] == 1);
    check("handshake length matches", be24(hello + 1) == hl - 4);
    check("legacy_version is 0x0303", be16(hello + 4) == 0x0303);
    check("random is copied", memcmp(hello + 6, random, 32) == 0);
    check("session id is copied", hello[38] == 32 && memcmp(hello + 39, sid, 32) == 0);

    size_t p = 71;
    check("cipher suite is TLS_CHACHA20_POLY1305_SHA256 alone",
          be16(hello + p) == 2 && be16(hello + p + 2) == 0x1303);
    p += 4;
    check("compression is null only", hello[p] == 1 && hello[p + 1] == 0);
    p += 2;
    size_t ext_total = be16(hello + p);
    p += 2;
    check("extensions run to the end of the message", p + ext_total == hl);

    int saw_sni = 0, saw_groups = 0, saw_sigs = 0, saw_versions = 0, saw_share = 0;
    int share_ok = 0, sni_ok = 0;
    size_t q = p;
    while (q < hl) {
        uint32_t type = be16(hello + q);
        uint32_t len = be16(hello + q + 2);
        const uint8_t *d = hello + q + 4;
        if (type == 0x0000) {
            saw_sni = 1;
            sni_ok = len == 16 && be16(d + 3) == 11 && memcmp(d + 5, "example.com", 11) == 0;
        } else if (type == 0x000a) {
            saw_groups = be16(d + 2) == 0x001d;
        } else if (type == 0x000d) {
            saw_sigs = len == 8;
        } else if (type == 0x002b) {
            saw_versions = d[0] == 2 && be16(d + 1) == 0x0304;
        } else if (type == 0x0033) {
            saw_share = 1;
            share_ok = be16(d + 2) == 0x001d && be16(d + 4) == 32 && memcmp(d + 6, pub, 32) == 0;
        }
        q += 4 + len;
    }
    check("SNI carries the host name", saw_sni && sni_ok);
    check("supported_groups offers x25519", saw_groups);
    check("signature_algorithms present", saw_sigs);
    check("supported_versions is TLS 1.3", saw_versions);
    check("key_share carries the x25519 public key", saw_share && share_ok);

    uint8_t small[16];
    check("too small a buffer is refused", tls13_build_client_hello(small, sizeof small, random, sid, pub, NULL) == 0);

    if (failures) {
        printf("tls13-record-unit: %d failure(s)\n", failures);
        return 1;
    }
    printf("tls13-record-unit: all checks passed\n");
    return 0;
}
