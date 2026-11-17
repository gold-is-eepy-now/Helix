/* net_tls.c — a TLS 1.3 client session over net_tcp. See net_tls.h.
 *
 * Order of work when a session opens:
 *   1. random values for the client (RDRAND only; refused without it)
 *   2. ClientHello out, then server messages in until the client reaches WAIT_AUTH
 *   3. the certificate chain and CertificateVerify are checked (tls13_verify.c)
 *   4. only after that, the application keys are copied into the session
 * Nothing that carries application data is sent before step 4 succeeds.
 *
 * Known difference: the client Finished is sent in step 2, before step 3. It carries no
 * application data, and the keys it is derived from are not released until step 3 passes. */
#include "net_tls.h"

#include "accounts.h"
#include "heap.h"
#include "kstring.h"
#include "net_tcp.h"
#include "tls13_client.h"
#include "tls13_record.h"
#include "tls13_verify.h"
#include "time.h"
#include "trust_roots.h"
#include "x509_verify.h"

#ifdef HELIX_TLS_TEST
/* Test builds only (make test): the CA that signs the local HTTPS test server, embedded
 * from build/tls-test/ca.der. The release image is built without HELIX_TLS_TEST, so this
 * root never reaches it. See tests/tls-test-ca.sh. */
extern const uint8_t helix_tls_test_ca[];
extern const size_t helix_tls_test_ca_len;
#endif

#define NET_TLS_TIMEOUT_MS 5000u
#define TLS_PLAIN_MAX      16384u                       /* RFC 8446 section 5.1 */
#define TLS_REC_MAX        (TLS_PLAIN_MAX + 256u + 5u)  /* plaintext + padding room + header */
#define TLS_IN_CAP         20000u                       /* holds one record, or handshake data */

struct net_tls {
    tls13_client_t *hs;       /* the handshake state; freed once the keys are copied */
    tls13_cipher_t tx, rx;    /* application keys */
    uint8_t *in;              /* bytes from TCP not yet consumed (also the handshake buffer) */
    size_t in_len;
    uint8_t *out;             /* one encrypted record being built */
    uint8_t *plain;           /* decrypted application data not yet returned */
    size_t plain_len, plain_pos;
};

static void set_err(char *err, size_t cap, const char *msg) {
    if (err && cap) {
        size_t n = strlen(msg);
        if (n >= cap) n = cap - 1;
        memcpy(err, msg, n);
        err[n] = '\0';
    }
}

static void session_free(net_tls_t *t) {
    if (t == NULL) return;
    if (t->hs) kfree(t->hs);
    if (t->in) kfree(t->in);
    if (t->out) kfree(t->out);
    if (t->plain) kfree(t->plain);
    kfree(t);
}

/* Sends whatever the handshake state machine has ready. */
static int flush_handshake(net_tls_t *t, int conn) {
    uint8_t out[512];
    size_t k = tls13_client_take_output(t->hs, out, sizeof out);
    if (k == 0) return 0;
    return net_tcp_send(conn, out, (uint32_t)k, NET_TLS_TIMEOUT_MS) < 0 ? -1 : 0;
}

net_tls_t *net_tls_open(int conn, const char *host, char *err, size_t err_cap) {
    bool used_rdrand = false;
    uint8_t random[32], session_id[32], priv[32];
    accounts_random(random, sizeof random, &used_rdrand);
    if (!used_rdrand) {
        set_err(err, err_cap, "no hardware random number generator (RDRAND); HTTPS is refused");
        return NULL;
    }
    accounts_random(session_id, sizeof session_id, &used_rdrand);
    accounts_random(priv, sizeof priv, &used_rdrand);

    net_tls_t *t = kmalloc(sizeof *t);
    if (t == NULL) { set_err(err, err_cap, "out of memory"); return NULL; }
    memset(t, 0, sizeof *t);
    t->hs = kmalloc(sizeof *t->hs);
    t->in = kmalloc(TLS_IN_CAP);
    t->out = kmalloc(TLS_REC_MAX);
    t->plain = kmalloc(TLS_REC_MAX);
    if (!t->hs || !t->in || !t->out || !t->plain) {
        session_free(t);
        set_err(err, err_cap, "out of memory");
        return NULL;
    }
    memset(t->hs, 0, sizeof *t->hs);
    tls13_client_init(t->hs, random, session_id, priv);

    /* ClientHello. The host name is sent as SNI and is checked against the certificate. */
    uint8_t hello[512];
    size_t hn = tls13_client_hello(t->hs, hello, sizeof hello, host);
    if (hn == 0 || net_tcp_send(conn, hello, (uint32_t)hn, NET_TLS_TIMEOUT_MS) < 0) {
        session_free(t);
        set_err(err, err_cap, "could not send the TLS ClientHello");
        return NULL;
    }

    /* Handshake. Server bytes accumulate in t->in; complete messages are consumed, and
     * any bytes after them (such as post-handshake messages) stay for net_tls_recv. */
    while (t->hs->state != TLS_ST_WAIT_AUTH) {
        if (t->hs->state == TLS_ST_FAILED) {
            session_free(t);
            set_err(err, err_cap, "TLS handshake failed (unexpected or malformed message)");
            return NULL;
        }
        if (t->in_len == TLS_IN_CAP) {
            session_free(t);
            set_err(err, err_cap, "TLS handshake message too large");
            return NULL;
        }
        int r = net_tcp_recv(conn, t->in + t->in_len, (uint32_t)(TLS_IN_CAP - t->in_len), NET_TLS_TIMEOUT_MS);
        if (r <= 0) {
            session_free(t);
            set_err(err, err_cap, "the server closed or timed out during the TLS handshake");
            return NULL;
        }
        t->in_len += (size_t)r;
        size_t used = tls13_client_feed(t->hs, t->in, t->in_len);
        if (t->hs->state == TLS_ST_FAILED) {
            session_free(t);
            set_err(err, err_cap, "TLS handshake failed (unexpected or malformed message)");
            return NULL;
        }
        memmove(t->in, t->in + used, t->in_len - used);
        t->in_len -= used;
        if (flush_handshake(t, conn) < 0) {
            session_free(t);
            set_err(err, err_cap, "could not send the TLS Finished");
            return NULL;
        }
    }

    /* Certificate chain and CertificateVerify. Refused without a clock: validity periods
     * cannot be checked, and an unchecked certificate is not accepted. */
    uint64_t now = time_epoch();
    if (now == 0) {
        session_free(t);
        set_err(err, err_cap, "no wall clock; certificate dates cannot be checked");
        return NULL;
    }
    x509_policy_t pol;
    pol.now = (int64_t)now;
    pol.host = host;
    pol.host_len = strlen(host);
    pol.roots = trust_roots;
    pol.root_count = trust_root_count;
    trust_root_t *extra_roots = NULL;
#ifdef HELIX_TLS_TEST
    /* Built-in roots plus the test CA, in one array the policy can read. */
    extra_roots = kmalloc((trust_root_count + 1) * sizeof *extra_roots);
    if (extra_roots == NULL) {
        session_free(t);
        set_err(err, err_cap, "out of memory");
        return NULL;
    }
    memcpy(extra_roots, trust_roots, trust_root_count * sizeof *extra_roots);
    extra_roots[trust_root_count].der = helix_tls_test_ca;
    extra_roots[trust_root_count].len = helix_tls_test_ca_len;
    pol.roots = extra_roots;
    pol.root_count = trust_root_count + 1;
#endif
    char why[96] = "";
    int verified = tls13_client_verify_server(t->hs, &pol, why, sizeof why);
    if (extra_roots) kfree(extra_roots);
    if (!verified) {
        session_free(t);
        char msg[160] = "certificate refused: ";
        size_t n = strlen(msg);
        size_t wl = strlen(why);
        if (wl > sizeof msg - n - 1) wl = sizeof msg - n - 1;
        memcpy(msg + n, why, wl);
        msg[n + wl] = '\0';
        set_err(err, err_cap, msg);
        return NULL;
    }

    if (!tls13_client_app_keys(t->hs, &t->tx, &t->rx)) {
        session_free(t);
        set_err(err, err_cap, "no application keys after the handshake");
        return NULL;
    }
    kfree(t->hs);
    t->hs = NULL;
    return t;
}

int net_tls_send(net_tls_t *t, int conn, const uint8_t *data, uint32_t len) {
    while (len > 0) {
        uint32_t chunk = len > TLS_PLAIN_MAX ? TLS_PLAIN_MAX : len;
        size_t rl = tls13_seal(t->out, &t->tx, 23 /* application_data */, data, chunk);
        if (rl == 0 || net_tcp_send(conn, t->out, (uint32_t)rl, NET_TLS_TIMEOUT_MS) < 0) return -1;
        data += chunk;
        len -= chunk;
    }
    return 0;
}

int net_tls_recv(net_tls_t *t, int conn, uint8_t *buf, uint32_t cap) {
    for (;;) {
        if (t->plain_pos < t->plain_len) {
            size_t avail = t->plain_len - t->plain_pos;
            size_t n = avail < cap ? avail : cap;
            memcpy(buf, t->plain + t->plain_pos, n);
            t->plain_pos += n;
            return (int)n;
        }

        /* Gather one whole record from TCP. */
        for (;;) {
            if (t->in_len >= 5) {
                size_t body = ((size_t)t->in[3] << 8) | t->in[4];
                if (body > TLS_REC_MAX) return -1;
                if (t->in_len >= 5 + body) break;
            }
            if (t->in_len == TLS_IN_CAP) return -1;
            int r = net_tcp_recv(conn, t->in + t->in_len, (uint32_t)(TLS_IN_CAP - t->in_len), NET_TLS_TIMEOUT_MS);
            if (r == 0) return t->in_len == 0 ? 0 : -1;   /* a record cut short is an error */
            if (r < 0) return -1;
            t->in_len += (size_t)r;
        }

        size_t rec_len = 5 + (((size_t)t->in[3] << 8) | t->in[4]);
        if (t->in[0] == 20) {
            /* ChangeCipherSpec: a compatibility message, ignored. */
        } else if (t->in[0] == 23) {
            uint8_t ct = 0;
            size_t pl = 0;
            if (!tls13_open(t->plain, &pl, &ct, &t->rx, t->in, rec_len)) return -1;
            if (ct == 23) {
                t->plain_len = pl;
                t->plain_pos = 0;
            } else if (ct == 21) {
                /* Alert: close_notify (level 1, description 0) is a clean end. */
                return (pl >= 2 && t->plain[1] == 0) ? 0 : -1;
            } else if (ct != 22) {
                return -1;                              /* handshake messages (tickets) are skipped */
            }
        } else {
            return -1;
        }
        memmove(t->in, t->in + rec_len, t->in_len - rec_len);
        t->in_len -= rec_len;
    }
}

void net_tls_close(net_tls_t *t) {
    session_free(t);
}
