/* net_dns.c — a DNS stub resolver: one A-record query, one answer (see net_dns.h).
 *
 * Query message (RFC 1035):
 *   0 id (2)   2 flags (2)   4 question count   6 answer count   8 authority   10 additional
 *   12 question: name as length-prefixed labels ending in a zero byte, then type, class.
 * The answer section repeats names, often as 2-byte pointers (top two bits 11) back into
 * the message; the parser follows those by skipping them, which is all it needs.
 */
#include "net_dns.h"

#include <stddef.h>

#include "klog.h"
#include "net.h"
#include "net_udp.h"
#include "time.h"

#define DNS_PORT         53
#define DNS_CLIENT_PORT  40053     /* our source port while a query is outstanding */
#define DNS_ID           0x4853    /* arbitrary: answers with another id are ignored */
#define DNS_MAX_MSG      512
#define DNS_ATTEMPTS     3
#define DNS_FLAG_RESPONSE 0x8000
#define DNS_RCODE_MASK   0x000F
#define DNS_TYPE_A       1
#define DNS_CLASS_IN     1

static void put16(uint8_t *p, uint16_t v) { net_put16(p, v); }

static int g_last_rcode;

/* Write `name` as DNS labels at out. Returns the bytes written, or 0 if the name is not
 * usable: empty labels, a label over 63 bytes, or a total over 255 bytes. */
static uint16_t encode_name(const char *name, uint8_t *out, uint16_t cap) {
    uint16_t total = 0;
    const char *p = name;
    if (*p == '\0') return 0;
    while (*p != '\0') {
        const char *end = p;
        while (*end != '\0' && *end != '.') end++;
        uint16_t len = (uint16_t)(end - p);
        if (len == 0 || len > 63) return 0;
        if (total + len + 2 > 255 || total + len + 2 > cap) return 0;
        out[total++] = (uint8_t)len;
        for (uint16_t i = 0; i < len; i++) {
            char c = p[i];
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');   /* DNS names are case-insensitive */
            out[total++] = (uint8_t)c;
        }
        p = end;
        if (*p == '.') {
            p++;
            if (*p == '\0') break;              /* a trailing dot is allowed */
        }
    }
    out[total++] = 0;                            /* the root label ends the name */
    return total;
}

/* Skip a (possibly compressed) name starting at off. Returns the offset just past it,
 * or -1 if the message ends inside it. */
static int skip_name(const uint8_t *msg, uint16_t len, int off) {
    while (off < (int)len) {
        uint8_t l = msg[off];
        if (l == 0) return off + 1;
        if ((l & 0xC0) == 0xC0) return off + 2;   /* pointer: two bytes, then done */
        if (l & 0xC0) return -1;                  /* 01 and 10 are reserved */
        off += 1 + l;
    }
    return -1;
}

/* Find the first A record in the answers. Returns NET_DNS_OK with out set, or
 * NET_DNS_NOTFOUND if there is none. */
static int parse_answer(const uint8_t *msg, uint16_t len, uint8_t out[4]) {
    if (len < 12) return NET_DNS_NOANSWER;
    if (net_get16(msg) != DNS_ID) return NET_DNS_NOANSWER;     /* not our answer */
    if (!(net_get16(msg + 2) & DNS_FLAG_RESPONSE)) return NET_DNS_NOANSWER;
    g_last_rcode = net_get16(msg + 2) & DNS_RCODE_MASK;
    if (g_last_rcode == 3) return NET_DNS_NOTFOUND;     /* NXDOMAIN: no such name */
    if (g_last_rcode != 0) return NET_DNS_SERVFAIL;     /* 2 = server failure, and others */

    uint16_t qd = net_get16(msg + 4);
    uint16_t an = net_get16(msg + 6);
    int off = 12;
    for (uint16_t i = 0; i < qd; i++) {
        off = skip_name(msg, len, off);
        if (off < 0 || off + 4 > len) return NET_DNS_NOANSWER;
        off += 4;                                              /* type and class */
    }
    for (uint16_t i = 0; i < an; i++) {
        off = skip_name(msg, len, off);
        if (off < 0 || off + 10 > len) return NET_DNS_NOANSWER;
        uint16_t type = net_get16(msg + off);
        uint16_t rdlen = net_get16(msg + off + 8);
        int rdata = off + 10;
        if (rdata + rdlen > len) return NET_DNS_NOANSWER;
        if (type == DNS_TYPE_A && rdlen == 4) {
            for (int k = 0; k < 4; k++) out[k] = msg[rdata + k];
            return NET_DNS_OK;
        }
        off = rdata + rdlen;
    }
    return NET_DNS_NOTFOUND;
}

int net_dns_last_rcode(void) { return g_last_rcode; }

int net_dns_resolve(const char *name, uint8_t out[4], uint32_t timeout_ms) {
    static uint8_t query[DNS_MAX_MSG];
    static uint8_t reply[DNS_MAX_MSG];
    if (!net_ready()) return NET_DNS_NOANSWER;

    uint16_t n = 12;
    put16(query, DNS_ID);
    put16(query + 2, 0x0100);          /* standard query, recursion desired */
    put16(query + 4, 1);               /* one question */
    put16(query + 6, 0);
    put16(query + 8, 0);
    put16(query + 10, 0);
    uint16_t name_len = encode_name(name, query + n, (uint16_t)(DNS_MAX_MSG - n - 4));
    if (name_len == 0) return NET_DNS_NOANSWER;
    n = (uint16_t)(n + name_len);
    put16(query + n, DNS_TYPE_A);
    put16(query + n + 2, DNS_CLASS_IN);
    n = (uint16_t)(n + 4);

    int slot = net_udp_bind(DNS_CLIENT_PORT);
    if (slot < 0) return NET_DNS_NOANSWER;
    const uint8_t *server = net_dns_server();
    int result = NET_DNS_NOANSWER;
    /* Each attempt waits its share of the total; a lost query is re-sent. */
    uint32_t per_attempt = timeout_ms / DNS_ATTEMPTS;
    if (per_attempt == 0) per_attempt = 1;
    for (int attempt = 0; attempt < DNS_ATTEMPTS; attempt++) {
        if (!net_udp_send(server, DNS_PORT, DNS_CLIENT_PORT, query, n)) break;
        uint64_t start = time_uptime_ms();
        for (;;) {
            uint8_t src[4];
            uint16_t sport = 0;
            int got = net_udp_recv(slot, src, &sport, reply, DNS_MAX_MSG, per_attempt);
            if (got < 0) break;                              /* this attempt timed out */
            if (sport != DNS_PORT) continue;                 /* only the server's answers */
            if (got > DNS_MAX_MSG) continue;
            int r = parse_answer(reply, (uint16_t)got, out);
            if (r != NET_DNS_NOANSWER) {
                result = r;
                goto done;
            }
            if (time_uptime_ms() - start >= per_attempt) break;
        }
    }
done:
    net_udp_close(slot);
    if (result == NET_DNS_NOANSWER) {
        klog_note(KLOG_INPUT, "[net] DNS: no answer for a query\n");
    }
    return result;
}
