/* net_tcp.c — TCP client connections (see net_tcp.h).
 *
 * TCP header, 20 bytes without options:
 *   0 source port  2 destination port  4 sequence  8 acknowledgement
 *   12 data offset (high 4 bits, in 32-bit words)  13 flags  14 window
 *   16 checksum (over a pseudo-header, the header and the data)  18 urgent pointer
 * Flags: FIN 0x01, SYN 0x02, RST 0x04, PSH 0x08, ACK 0x10.
 *
 * Sequence numbers count bytes. A SYN or FIN takes one sequence number of its own.
 * Each connection keeps a receive ring; send() keeps the caller's buffer, because it
 * does not return until everything is acknowledged, so retransmission reads it again.
 */
#include "net_tcp.h"

#include <stddef.h>

#include "klog.h"
#include "net.h"
#include "net_ip.h"
#include "time.h"

#define TCP_HLEN       20
#define TCP_MSS        1400         /* payload per segment: fits one frame with headers */
#define TCP_RX_BUF     8192         /* receive ring per connection; also the window we offer */
#define TCP_RTO_MS     1000         /* retransmission timeout */
#define TCP_MAX_RETRY  5
#define TCP_CONNECT_RETRY_MS 1000

#define F_FIN 0x01
#define F_SYN 0x02
#define F_RST 0x04
#define F_PSH 0x08
#define F_ACK 0x10

typedef enum {
    TC_FREE,
    TC_SYN_SENT,
    TC_ESTABLISHED,
    TC_FIN_WAIT,        /* we sent FIN, waiting for the peer's FIN */
    TC_CLOSE_WAIT,      /* the peer sent FIN; we can still send */
    TC_RESET,
    TC_SYN_RCVD        /* passive open: we sent SYN-ACK, waiting for the peer's ACK */
} tcp_state_t;

#define TC_SYN_RCVD_TIMEOUT_MS 5000   /* a half-open connection is dropped after this */

typedef struct {
    tcp_state_t state;
    bool passive;          /* created by a listener, not by connect() */
    bool accepted;         /* handed to accept() */
    uint64_t created_ms;   /* when a passive connection started its handshake */
    uint8_t peer_ip[4];
    uint16_t peer_port;
    uint16_t local_port;
    uint32_t iss;           /* our initial sequence number */
    uint32_t snd_una;       /* oldest unacknowledged sequence number */
    uint32_t snd_nxt;       /* next sequence number to send (rewound on retransmit) */
    uint32_t snd_max;       /* highest sequence number ever sent; ACKs may cover up to here */
    uint32_t rcv_nxt;       /* next sequence number we expect from the peer */
    uint32_t peer_wnd;      /* the peer's advertised window */
    uint8_t rx[TCP_RX_BUF];
    uint32_t rx_head;       /* index of the oldest byte in rx */
    uint32_t rx_count;      /* bytes waiting in rx */
} tcp_conn_t;

static tcp_conn_t g_conns[NET_TCP_CONNS];
static uint16_t g_next_port = 49152;
static uint32_t g_iss_seed = 0x1000;

/* Listening ports. A SYN to one of these creates a passive connection in a free slot. */
typedef struct {
    bool used;
    uint16_t port;
} tcp_listener_t;

static tcp_listener_t g_listeners[NET_TCP_LISTENERS];

static bool seq_after(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) > 0;          /* correct across the 2^32 wrap */
}

static bool same4(const uint8_t *a, const uint8_t *b) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

static uint32_t get32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* The TCP checksum: the Internet checksum over a pseudo-header (source address,
 * destination address, zero, protocol 6, segment length) followed by the segment.
 * The segment's own checksum field (bytes 16-17) is left out of the sum. Summed in
 * place with no copy, so a segment of any length up to the frame size works. Returns
 * the value that belongs in the field. */
static uint16_t tcp_checksum(const uint8_t src[4], const uint8_t dst[4], const uint8_t *seg, uint16_t len) {
    uint32_t sum = 0;
    sum += ((uint32_t)src[0] << 8) | src[1];
    sum += ((uint32_t)src[2] << 8) | src[3];
    sum += ((uint32_t)dst[0] << 8) | dst[1];
    sum += ((uint32_t)dst[2] << 8) | dst[3];
    sum += NET_IP_PROTO_TCP;
    sum += len;
    for (uint32_t i = 0; i + 1 < len; i += 2) {
        if (i == 16) continue;                       /* the checksum field itself */
        sum += ((uint32_t)seg[i] << 8) | seg[i + 1];
    }
    if (len & 1) sum += (uint32_t)seg[len - 1] << 8; /* odd length: pad with a zero byte */
    while (sum >> 16) sum = (sum & 0xffffu) + (sum >> 16);
    return (uint16_t)~sum;
}

/* Build and send one segment. payload may be NULL when len is 0. */
static bool send_segment(tcp_conn_t *c, uint8_t flags, uint32_t seq, const uint8_t *payload, uint16_t len) {
    /* Resolve the next hop first: ARP may poll the network, and a poll can send an ACK
     * through this function. Resolving first keeps the static buffer below unshared. */
    if (!net_ip_warm(c->peer_ip)) return false;
    static uint8_t seg[TCP_HLEN + TCP_MSS];
    uint32_t space = TCP_RX_BUF - c->rx_count;
    uint16_t wnd = space > 65535u ? 65535u : (uint16_t)space;

    net_put16(seg, c->local_port);
    net_put16(seg + 2, c->peer_port);
    put32(seg + 4, seq);
    put32(seg + 8, (flags & F_ACK) ? c->rcv_nxt : 0);
    seg[12] = (uint8_t)((TCP_HLEN / 4) << 4);
    seg[13] = flags;
    net_put16(seg + 14, wnd);
    net_put16(seg + 16, 0);
    net_put16(seg + 18, 0);
    for (uint16_t i = 0; i < len; i++) seg[TCP_HLEN + i] = payload[i];
    net_put16(seg + 16, tcp_checksum(net_ip_addr(), c->peer_ip, seg, (uint16_t)(TCP_HLEN + len)));
    return net_ip_send(c->peer_ip, NET_IP_PROTO_TCP, seg, (uint16_t)(TCP_HLEN + len));
}

static tcp_conn_t *conn_at(int id) {
    if (id < 0 || id >= NET_TCP_CONNS) return NULL;
    if (g_conns[id].state == TC_FREE) return NULL;
    return &g_conns[id];
}

static void free_conn(tcp_conn_t *c) {
    c->state = TC_FREE;
    c->rx_head = 0;
    c->rx_count = 0;
}

static uint16_t alloc_port(void) {
    g_next_port++;
    if (g_next_port < 49152) g_next_port = 49152;
    return g_next_port;
}

static uint32_t next_iss(void) {
    g_iss_seed += 0x10000u + (uint32_t)time_tsc();     /* not predictable, not secret: a demo */
    return g_iss_seed;
}

int net_tcp_connect(const uint8_t ip[4], uint16_t port, uint32_t timeout_ms) {
    if (!net_ready()) return -1;
    int id = -1;
    for (int i = 0; i < NET_TCP_CONNS; i++) {
        if (g_conns[i].state == TC_FREE) { id = i; break; }
    }
    if (id < 0) return -1;
    tcp_conn_t *c = &g_conns[id];
    c->state = TC_SYN_SENT;
    c->passive = false;
    c->accepted = false;
    for (int i = 0; i < 4; i++) c->peer_ip[i] = ip[i];
    c->peer_port = port;
    c->local_port = alloc_port();
    c->iss = next_iss();
    c->snd_una = c->iss;
    c->snd_nxt = c->iss + 1;             /* the SYN takes one sequence number */
    c->snd_max = c->snd_nxt;
    c->rcv_nxt = 0;
    c->peer_wnd = 0;
    c->rx_head = 0;
    c->rx_count = 0;

    uint64_t start = time_uptime_ms();
    uint64_t next_syn = start;
    for (;;) {
        uint64_t now = time_uptime_ms();
        if (now >= next_syn) {
            send_segment(c, F_SYN, c->iss, NULL, 0);
            next_syn = now + TCP_CONNECT_RETRY_MS;
        }
        net_poll();
        if (c->state == TC_ESTABLISHED) return id;
        if (c->state == TC_RESET || c->state == TC_FREE) break;
        if (now - start >= timeout_ms) break;
    }
    klog_note(KLOG_INPUT, "[net] TCP connect to port %u failed\n", port);
    free_conn(c);
    return -1;
}

int net_tcp_listen(uint16_t port) {
    if (!net_ready() || port == 0) return -1;
    int free_slot = -1;
    for (int i = 0; i < NET_TCP_LISTENERS; i++) {
        if (g_listeners[i].used) {
            if (g_listeners[i].port == port) return -1;     /* already listening */
        } else if (free_slot < 0) {
            free_slot = i;
        }
    }
    if (free_slot < 0) return -1;
    g_listeners[free_slot].used = true;
    g_listeners[free_slot].port = port;
    return free_slot;
}

void net_tcp_unlisten(int listener) {
    if (listener < 0 || listener >= NET_TCP_LISTENERS) return;
    g_listeners[listener].used = false;
}

/* Drop passive connections on this port whose handshake never finished. */
static void expire_half_open(uint16_t port) {
    uint64_t now = time_uptime_ms();
    for (int i = 0; i < NET_TCP_CONNS; i++) {
        tcp_conn_t *c = &g_conns[i];
        if (c->state == TC_SYN_RCVD && c->passive && c->local_port == port &&
            now - c->created_ms >= TC_SYN_RCVD_TIMEOUT_MS) {
            free_conn(c);
        }
    }
}

int net_tcp_accept(int listener, uint32_t timeout_ms) {
    if (listener < 0 || listener >= NET_TCP_LISTENERS || !g_listeners[listener].used) return -1;
    uint16_t port = g_listeners[listener].port;
    uint64_t start = time_uptime_ms();
    for (;;) {
        net_poll();
        for (int i = 0; i < NET_TCP_CONNS; i++) {
            tcp_conn_t *c = &g_conns[i];
            /* CLOSE_WAIT counts too: the peer may have sent and closed before we looked. */
            if (c->passive && c->local_port == port && !c->accepted &&
                (c->state == TC_ESTABLISHED || c->state == TC_CLOSE_WAIT)) {
                c->accepted = true;
                return i;
            }
        }
        expire_half_open(port);
        if (time_uptime_ms() - start >= timeout_ms) return -1;
    }
}

int net_tcp_send(int id, const uint8_t *data, uint32_t len, uint32_t timeout_ms) {
    tcp_conn_t *c = conn_at(id);
    if (!c || (c->state != TC_ESTABLISHED && c->state != TC_CLOSE_WAIT)) return -1;
    uint32_t base = c->snd_nxt;          /* sequence number of data[0] */
    uint64_t start = time_uptime_ms();
    uint64_t last_progress = start;
    uint32_t acked_before = c->snd_una;
    int retries = 0;

    while (c->snd_una - base < len) {
        if (c->state != TC_ESTABLISHED && c->state != TC_CLOSE_WAIT) return -1;
        uint32_t sent = c->snd_nxt - base;
        uint32_t unsent = len - sent;
        uint32_t inflight = c->snd_nxt - c->snd_una;
        uint32_t room = c->peer_wnd > inflight ? c->peer_wnd - inflight : 0;
        if (unsent > 0 && room > 0) {
            uint32_t n = unsent;
            if (n > TCP_MSS) n = TCP_MSS;
            if (n > room) n = room;
            if (!send_segment(c, F_ACK | F_PSH, c->snd_nxt, data + sent, (uint16_t)n)) return -1;
            c->snd_nxt += n;
            if (seq_after(c->snd_nxt, c->snd_max)) c->snd_max = c->snd_nxt;
            last_progress = time_uptime_ms();
            continue;
        }
        net_poll();
        if (c->snd_una != acked_before) {          /* the peer acknowledged something */
            acked_before = c->snd_una;
            last_progress = time_uptime_ms();
            retries = 0;
        }
        uint64_t now = time_uptime_ms();
        if (now - start >= timeout_ms) return -1;
        if (now - last_progress >= TCP_RTO_MS) {
            if (++retries > TCP_MAX_RETRY) return -1;
            c->snd_nxt = c->snd_una;               /* go back: resend from the first unacked byte */
            last_progress = now;
        }
    }
    return (int)len;
}

int net_tcp_recv(int id, uint8_t *buf, uint32_t cap, uint32_t timeout_ms) {
    tcp_conn_t *c = conn_at(id);
    if (!c) return -1;
    uint64_t start = time_uptime_ms();
    for (;;) {
        if (c->rx_count > 0) {
            uint32_t n = c->rx_count < cap ? c->rx_count : cap;
            for (uint32_t i = 0; i < n; i++) buf[i] = c->rx[(c->rx_head + i) % TCP_RX_BUF];
            c->rx_head = (c->rx_head + n) % TCP_RX_BUF;
            c->rx_count -= n;
            /* Freed space: tell the peer the window opened. Without this a peer that
             * filled our window waits for its probe timer, and the stall outlasts timeouts. */
            if (c->state == TC_ESTABLISHED || c->state == TC_CLOSE_WAIT) {
                send_segment(c, F_ACK, c->snd_nxt, NULL, 0);
            }
            return (int)n;
        }
        if (c->state == TC_CLOSE_WAIT || c->state == TC_FIN_WAIT) return 0;   /* peer closed */
        if (c->state == TC_RESET || c->state == TC_FREE) return -1;
        net_poll();
        if (time_uptime_ms() - start >= timeout_ms) return -1;
    }
}

void net_tcp_close(int id) {
    tcp_conn_t *c = conn_at(id);
    if (!c) return;
    if (c->state == TC_ESTABLISHED || c->state == TC_CLOSE_WAIT) {
        send_segment(c, F_FIN | F_ACK, c->snd_nxt, NULL, 0);
        c->snd_nxt++;                       /* the FIN takes one sequence number */
        if (seq_after(c->snd_nxt, c->snd_max)) c->snd_max = c->snd_nxt;
        c->state = TC_FIN_WAIT;
        uint64_t start = time_uptime_ms();
        /* Wait briefly for the peer's FIN, which closes our side cleanly. */
        while (c->state == TC_FIN_WAIT && time_uptime_ms() - start < 1000) net_poll();
    }
    free_conn(c);
}

static bool listening_on(uint16_t port) {
    for (int i = 0; i < NET_TCP_LISTENERS; i++) {
        if (g_listeners[i].used && g_listeners[i].port == port) return true;
    }
    return false;
}

/* A SYN arrived for a listening port: take a free slot and answer SYN-ACK. If every slot
 * is busy the SYN is dropped, and the peer retries it later. */
static void open_passive(const uint8_t src[4], uint16_t sport, uint16_t dport, uint32_t seq, uint16_t wnd) {
    tcp_conn_t *c = NULL;
    for (int i = 0; i < NET_TCP_CONNS; i++) {
        if (g_conns[i].state == TC_FREE) { c = &g_conns[i]; break; }
    }
    if (!c) return;
    c->state = TC_SYN_RCVD;
    c->passive = true;
    c->accepted = false;
    c->created_ms = time_uptime_ms();
    for (int i = 0; i < 4; i++) c->peer_ip[i] = src[i];
    c->peer_port = sport;
    c->local_port = dport;
    c->iss = next_iss();
    c->snd_una = c->iss;
    c->snd_nxt = c->iss + 1;             /* the SYN-ACK takes one sequence number */
    c->snd_max = c->snd_nxt;
    c->rcv_nxt = seq + 1;                /* the peer's SYN takes one sequence number */
    c->peer_wnd = wnd;
    c->rx_head = 0;
    c->rx_count = 0;
    send_segment(c, F_SYN | F_ACK, c->iss, NULL, 0);
}

void net_tcp_input(const uint8_t src[4], const uint8_t dst[4], const uint8_t *seg, uint16_t len) {
    if (len < TCP_HLEN) return;
    uint8_t hl = (uint8_t)((seg[12] >> 4) * 4);
    if (hl < TCP_HLEN || hl > len) return;
    /* tcp_checksum returns the value that should be in the field; compare with what is there. */
    if (tcp_checksum(src, dst, seg, len) != net_get16(seg + 16)) return;   /* damaged segment */

    uint16_t sport = net_get16(seg);
    uint16_t dport = net_get16(seg + 2);
    uint32_t seq = get32(seg + 4);
    uint32_t ack = get32(seg + 8);
    uint8_t flags = seg[13];
    uint16_t wnd = net_get16(seg + 14);
    uint16_t plen = (uint16_t)(len - hl);
    const uint8_t *payload = seg + hl;

    tcp_conn_t *c = NULL;
    for (int i = 0; i < NET_TCP_CONNS; i++) {
        tcp_conn_t *k = &g_conns[i];
        if (k->state != TC_FREE && k->local_port == dport && k->peer_port == sport &&
            same4(k->peer_ip, src)) {
            c = k;
            break;
        }
    }
    if (!c) {
        /* A bare SYN to a port someone listens on starts a passive open. */
        if ((flags & (F_SYN | F_ACK | F_RST)) == F_SYN && listening_on(dport)) {
            open_passive(src, sport, dport, seq, wnd);
        }
        return;                                           /* otherwise not one of ours */
    }

    if (flags & F_RST) {
        c->state = TC_RESET;
        return;
    }

    if (c->state == TC_SYN_RCVD) {
        if (flags & F_SYN) {
            /* The peer resent its SYN, so our SYN-ACK was lost: send it again. */
            send_segment(c, F_SYN | F_ACK, c->iss, NULL, 0);
            return;
        }
        /* Only the ACK of our SYN-ACK completes the handshake. Anything else waits. */
        if (!(flags & F_ACK) || ack != c->iss + 1) return;
        c->snd_una = ack;
        c->peer_wnd = wnd;
        c->state = TC_ESTABLISHED;
        /* Fall through: this segment may already carry data or a FIN. */
    }

    if (c->state == TC_SYN_SENT) {
        if ((flags & (F_SYN | F_ACK)) == (F_SYN | F_ACK) && ack == c->iss + 1) {
            c->rcv_nxt = seq + 1;
            c->snd_una = ack;
            c->peer_wnd = wnd;
            c->state = TC_ESTABLISHED;
            send_segment(c, F_ACK, c->snd_nxt, NULL, 0);
        }
        return;
    }

    /* Acknowledgements: accept only those that cover data we actually sent. */
    if (flags & F_ACK) {
        uint32_t newly = ack - c->snd_una;
        uint32_t sent = c->snd_max - c->snd_una;
        if (newly != 0 && newly <= sent) {
            c->snd_una = ack;
            if (seq_after(ack, c->snd_nxt)) c->snd_nxt = ack;   /* peer has our data after a rewind */
        }
        c->peer_wnd = wnd;
    }

    /* Payload: only the next expected bytes are kept; anything else is re-acknowledged. */
    bool wants_ack = false;
    if (c->state == TC_ESTABLISHED || c->state == TC_FIN_WAIT) {
        /* Data, or a segment that is not the next one we expect (a duplicate, a window
         * probe, or a lost segment before it): answer with our current acknowledgement
         * so the peer learns where we are and our window. Pure in-sequence ACKs need none. */
        if (plen > 0 || seq != c->rcv_nxt) wants_ack = true;
        if (plen > 0 && seq == c->rcv_nxt) {
            uint32_t space = TCP_RX_BUF - c->rx_count;
            uint32_t n = plen < space ? plen : space;
            for (uint32_t i = 0; i < n; i++) {
                c->rx[(c->rx_head + c->rx_count + i) % TCP_RX_BUF] = payload[i];
            }
            c->rx_count += n;
            c->rcv_nxt += n;
        }
    }

    /* FIN: the peer is done sending once the bytes before it have arrived. */
    if ((flags & F_FIN) && seq + plen == c->rcv_nxt) {
        c->rcv_nxt++;
        wants_ack = true;
        if (c->state == TC_ESTABLISHED) c->state = TC_CLOSE_WAIT;
        else if (c->state == TC_FIN_WAIT) c->state = TC_CLOSE_WAIT;   /* our close() frees it */
    }
    if (wants_ack) send_segment(c, F_ACK, c->snd_nxt, NULL, 0);
}
