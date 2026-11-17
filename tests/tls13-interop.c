/* tls13-interop.c — host-only check of the kernel's TLS 1.3 client against a real server
 * (OpenSSL s_server, started by tests/tls13-interop.sh). It runs the handshake over TCP,
 * verifies the server's certificate chain against the root given on the command line
 * (tls13_client_verify_server, the same code the kernel uses), then sends a GET and prints
 * the status line.
 * Usage: tls13-interop <port> <root.der> <host>
 * Exit 0: verified and a 200 came back over encrypted records.
 * Exit 3: the server was refused by certification (the expected result for a wrong root). */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>


#include "tls13_client.h"
#include "tls13_record.h"
#include "tls13_verify.h"
#include "trust_roots.h"

static void random_bytes(uint8_t *b, size_t n) {
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f || fread(b, 1, n, f) != n) { fprintf(stderr, "no entropy\n"); exit(2); }
    fclose(f);
}

static int send_all(int fd, const uint8_t *b, size_t n) {
    while (n > 0) {
        ssize_t w = send(fd, b, n, 0);
        if (w <= 0) return 0;
        b += w;
        n -= (size_t)w;
    }
    return 1;
}

int main(int argc, char **argv) {
    if (argc != 4) { fprintf(stderr, "usage: %s <port> <root.der> <host>\n", argv[0]); return 2; }
    setvbuf(stdout, NULL, _IONBF, 0);            /* unbuffered: progress shows even if killed */

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = { 0 };
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)atoi(argv[1]));
    sa.sin_addr.s_addr = htonl(0x7f000001);
    if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) { perror("connect"); return 1; }

    tls13_client_t *c = calloc(1, sizeof *c);
    uint8_t random[32], sid[32], priv[32];
    random_bytes(random, 32);
    random_bytes(sid, 32);
    random_bytes(priv, 32);
    tls13_client_init(c, random, sid, priv);

    uint8_t out[1024];
    size_t n = tls13_client_hello(c, out, sizeof out, argv[3]);
    if (n == 0 || !send_all(fd, out, n)) { fprintf(stderr, "hello failed\n"); return 1; }

    /* Handshake: feed server bytes until the client reaches WAIT_AUTH. */
    static uint8_t acc[65536];
    size_t acc_len = 0;
    while (c->state != TLS_ST_WAIT_AUTH) {
        if (c->state == TLS_ST_FAILED) { fprintf(stderr, "handshake failed\n"); return 1; }
        ssize_t r = recv(fd, acc + acc_len, sizeof acc - acc_len, 0);
        if (r <= 0) { fprintf(stderr, "server closed during handshake\n"); return 1; }
        acc_len += (size_t)r;
        size_t used = tls13_client_feed(c, acc, acc_len);
        if (c->state == TLS_ST_FAILED) { fprintf(stderr, "handshake failed\n"); return 1; }
        memmove(acc, acc + used, acc_len - used);
        acc_len -= used;
        size_t k = tls13_client_take_output(c, out, sizeof out);
        if (k > 0 && !send_all(fd, out, k)) { fprintf(stderr, "send failed\n"); return 1; }
    }
    printf("interop: handshake reached WAIT_AUTH with the server's Finished verified\n");

    /* Certificate verification: the chain against the trust root, and the CertificateVerify
     * signature with the leaf key. Only then are application keys released. */
    static uint8_t root_der[8192];
    FILE *rf = fopen(argv[2], "rb");
    if (!rf) { perror("root"); return 2; }
    size_t root_len = fread(root_der, 1, sizeof root_der, rf);
    fclose(rf);
    trust_root_t roots[1] = { { root_der, root_len } };
    /* Wall time for validity checks. <time.h> is not used here: the kernel's own time.h
     * is on the include path and would shadow the host one. */
    struct timeval now_tv;
    gettimeofday(&now_tv, NULL);
    x509_policy_t pol = { (int64_t)now_tv.tv_sec, argv[3], strlen(argv[3]), roots, 1 };
    char err[96] = "";
    if (!tls13_client_verify_server(c, &pol, err, sizeof err)) {
        printf("interop: server refused: %s\n", err);
        return 3;
    }
    printf("interop: certificate chain and CertificateVerify verified for %s\n", argv[3]);

    tls13_cipher_t tx, rx;
    if (!tls13_client_app_keys(c, &tx, &rx)) { fprintf(stderr, "no app keys\n"); return 1; }

    const char *req = "GET / HTTP/1.0\r\nHost: localhost\r\n\r\n";
    uint8_t rec[4096];
    size_t rl = tls13_seal(rec, &tx, 23 /* application_data */, (const uint8_t *)req, strlen(req));
    if (!send_all(fd, rec, rl)) { fprintf(stderr, "request send failed\n"); return 1; }

    /* Read records until close, or until the server goes quiet for two seconds (s_server
     * -www keeps the connection open after its reply). Post-handshake messages are skipped. */
    struct timeval tv = { 2, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    static uint8_t resp[65536];
    size_t resp_len = 0;
    acc_len = 0;
    for (;;) {
        ssize_t r = recv(fd, acc + acc_len, sizeof acc - acc_len, 0);
        if (r <= 0) break;
        acc_len += (size_t)r;
        while (acc_len >= 5) {
            size_t body = ((size_t)acc[3] << 8) | acc[4];
            if (acc_len < 5 + body) break;
            uint8_t plain[65536];
            size_t plain_len = 0;
            uint8_t ct = 0;
            if (acc[0] == 23 && tls13_open(plain, &plain_len, &ct, &rx, acc, 5 + body)) {
                if (ct == 23 && resp_len + plain_len < sizeof resp) {
                    memcpy(resp + resp_len, plain, plain_len);
                    resp_len += plain_len;
                }
            } else if (acc[0] != 20) {          /* ChangeCipherSpec is ignored; others are errors */
                fprintf(stderr, "unexpected record type %u\n", acc[0]);
                return 1;
            }
            memmove(acc, acc + 5 + body, acc_len - 5 - body);
            acc_len -= 5 + body;
        }
    }
    resp[resp_len] = '\0';
    printf("interop: %.*s\n", 40 < (int)resp_len ? 40 : (int)resp_len, resp);
    close(fd);
    return strncmp((char *)resp, "HTTP/1.0 200", 12) == 0 || strncmp((char *)resp, "HTTP/1.1 200", 12) == 0 ? 0 : 1;
}
