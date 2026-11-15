/* net_http.c — an HTTP/1.1 GET client (see net_http.h).
 *
 * Steps: parse the URL, resolve the host name (or read a dotted address), connect,
 * send the request, read until the peer closes, then split the response at the blank
 * line that ends the headers. The body is framed one of three ways:
 *   Transfer-Encoding: chunked   each chunk is "<hex size>CRLF<data>CRLF", ended by size 0
 *   Content-Length: N            exactly N bytes
 *   neither                      everything up to the close
 * Chunked data is copied forward over its own framing, so the output never overtakes
 * the input and the body can be decoded in the same buffer. */
#include "net_http.h"

#include <stddef.h>

#include "kstring.h"
#include "net.h"
#include "net_dns.h"
#include "net_tcp.h"

#define HTTP_TIMEOUT_MS 5000
#define HTTP_DNS_MS     3000
#define HTTP_REQ_CAP    512
#define HTTP_PATH_CAP   256
#define HTTP_HOST_CAP   128

static void set_error(net_http_result_t *r, const char *msg) {
    uint32_t i = 0;
    while (msg[i] != '\0' && i + 1 < sizeof r->error) {
        r->error[i] = msg[i];
        i++;
    }
    r->error[i] = '\0';
}

static char lower(uint8_t c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : (char)c;
}

/* True when the n bytes at p begin with `name`, ignoring case. name must be lowercase. */
static bool starts_ci(const uint8_t *p, uint32_t n, const char *name) {
    for (uint32_t k = 0; name[k] != '\0'; k++) {
        if (k >= n || lower(p[k]) != name[k]) return false;
    }
    return true;
}

static int hexval(uint8_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Copy a header value: skip the spaces after the colon, drop trailing spaces and CR. */
static void copy_value(const uint8_t *line, uint32_t n, uint32_t after_colon, char *dst, uint32_t cap) {
    uint32_t a = after_colon, b = n;
    while (a < b && line[a] == ' ') a++;
    while (b > a && (line[b - 1] == ' ' || line[b - 1] == '\r')) b--;
    uint32_t k = 0;
    while (a + k < b && k + 1 < cap) {
        dst[k] = (char)line[a + k];
        k++;
    }
    dst[k] = '\0';
}

static bool parse_url(const char *url, char *host, uint32_t host_cap, uint16_t *port,
                      char *path, uint32_t path_cap, net_http_result_t *r) {
    if (strncmp(url, "https://", 8) == 0) {
        set_error(r, "https is not supported yet");
        return false;
    }
    if (strncmp(url, "http://", 7) != 0) {
        set_error(r, "the address must start with http://");
        return false;
    }
    const char *p = url + 7;
    uint32_t hn = 0;
    while (*p != '\0' && *p != ':' && *p != '/') {
        if (hn + 1 >= host_cap) {
            set_error(r, "host name too long");
            return false;
        }
        host[hn++] = *p++;
    }
    host[hn] = '\0';
    if (hn == 0) {
        set_error(r, "no host name in the address");
        return false;
    }
    uint32_t pv = 80;
    if (*p == ':') {
        p++;
        pv = 0;
        uint32_t digits = 0;
        while (*p >= '0' && *p <= '9' && pv <= 65535u) {
            pv = pv * 10u + (uint32_t)(*p - '0');
            p++;
            digits++;
        }
        if (digits == 0 || pv == 0 || pv > 65535u) {
            set_error(r, "bad port number");
            return false;
        }
    }
    *port = (uint16_t)pv;
    if (*p != '\0' && *p != '/') {
        set_error(r, "bad address");
        return false;
    }
    uint32_t pn = 0;
    if (*p == '\0') path[pn++] = '/';
    while (*p != '\0') {
        if (pn + 1 >= path_cap) {
            set_error(r, "address too long");
            return false;
        }
        path[pn++] = *p++;
    }
    path[pn] = '\0';
    return true;
}

static bool resolve_host(const char *host, uint8_t ip[4], net_http_result_t *r) {
    if (net_parse_ip(host, ip)) return true;
    int rc = net_dns_resolve(host, ip, HTTP_DNS_MS);
    if (rc == NET_DNS_OK) return true;
    if (rc == NET_DNS_NOTFOUND) set_error(r, "no such host");
    else if (rc == NET_DNS_SERVFAIL) set_error(r, "the DNS server failed");
    else set_error(r, "no answer from the DNS server");
    return false;
}

/* Append s to the request, failing when it would not fit. */
static bool append(char *d, uint32_t *n, uint32_t cap, const char *s) {
    while (*s != '\0') {
        if (*n + 1 >= cap) return false;
        d[(*n)++] = *s++;
    }
    d[*n] = '\0';
    return true;
}

static void read_header(const uint8_t *line, uint32_t n, net_http_result_t *r,
                        bool *chunked, bool *has_len, uint32_t *len) {
    if (starts_ci(line, n, "content-length:")) {
        uint32_t v = 0, digits = 0;
        uint32_t p = 15;
        while (p < n && line[p] == ' ') p++;
        while (p < n && line[p] >= '0' && line[p] <= '9' && v <= 0x7fffffffu) {
            v = v * 10u + (uint32_t)(line[p] - '0');
            p++;
            digits++;
        }
        if (digits > 0) {
            *len = v;
            *has_len = true;
        }
    } else if (starts_ci(line, n, "transfer-encoding:")) {
        char value[32];
        copy_value(line, n, 18, value, sizeof value);
        if (starts_ci((const uint8_t *)value, (uint32_t)strlen(value), "chunked")) *chunked = true;
    } else if (starts_ci(line, n, "location:")) {
        copy_value(line, n, 9, r->location, sizeof r->location);
    } else if (starts_ci(line, n, "content-type:")) {
        copy_value(line, n, 13, r->content_type, sizeof r->content_type);
    }
}

/* Decode chunked framing in place. On success *out_len is the body length, at body[0]. */
static bool decode_chunked(uint8_t *body, uint32_t avail, uint32_t *out_len, net_http_result_t *r) {
    uint32_t rd = 0, wr = 0;
    for (;;) {
        uint32_t size = 0, digits = 0;
        while (rd < avail && hexval(body[rd]) >= 0) {
            if (++digits > 8) {
                set_error(r, "bad chunk size");
                return false;
            }
            size = size * 16u + (uint32_t)hexval(body[rd]);
            rd++;
        }
        if (digits == 0) {
            set_error(r, "bad chunk size");
            return false;
        }
        /* Chunk extensions (";...") are ignored: skip to the end of the size line. */
        while (rd + 1 < avail && !(body[rd] == '\r' && body[rd + 1] == '\n')) rd++;
        if (rd + 1 >= avail) {
            set_error(r, "the chunked body was cut short");
            return false;
        }
        rd += 2;
        if (size == 0) break;                     /* the last chunk; trailers are ignored */
        if (size > avail - rd || avail - rd - size < 2) {
            set_error(r, "the chunked body was cut short");
            return false;
        }
        for (uint32_t k = 0; k < size; k++) body[wr + k] = body[rd + k];   /* forward: wr <= rd */
        wr += size;
        rd += size;
        if (body[rd] != '\r' || body[rd + 1] != '\n') {
            set_error(r, "bad chunk framing");
            return false;
        }
        rd += 2;
    }
    *out_len = wr;
    return true;
}

bool net_http_get(const char *url, uint8_t *buf, uint32_t cap, net_http_result_t *r) {
    r->status = 0;
    r->location[0] = '\0';
    r->content_type[0] = '\0';
    r->body = NULL;
    r->body_len = 0;
    r->error[0] = '\0';
    if (!net_ready()) {
        set_error(r, "no network card");
        return false;
    }
    char host[HTTP_HOST_CAP], path[HTTP_PATH_CAP];
    uint16_t port = 80;
    uint8_t ip[4];
    if (!parse_url(url, host, sizeof host, &port, path, sizeof path, r)) return false;
    if (!resolve_host(host, ip, r)) return false;

    int conn = net_tcp_connect(ip, port, HTTP_TIMEOUT_MS);
    if (conn < 0) {
        set_error(r, "connect failed");
        return false;
    }

    char req[HTTP_REQ_CAP];
    uint32_t qn = 0;
    if (!append(req, &qn, sizeof req, "GET ") || !append(req, &qn, sizeof req, path) ||
        !append(req, &qn, sizeof req, " HTTP/1.1\r\nHost: ") || !append(req, &qn, sizeof req, host) ||
        !append(req, &qn, sizeof req,
                "\r\nUser-Agent: Helix/0.5\r\nAccept: text/html, text/plain, */*\r\n"
                "Connection: close\r\n\r\n")) {
        net_tcp_close(conn);
        set_error(r, "request too long");
        return false;
    }
    if (net_tcp_send(conn, (const uint8_t *)req, qn, HTTP_TIMEOUT_MS) < 0) {
        net_tcp_close(conn);
        set_error(r, "send failed");
        return false;
    }

    /* Read until the peer closes. The buffer is the whole response, headers included. */
    uint32_t got = 0;
    for (;;) {
        if (got >= cap) {
            net_tcp_close(conn);
            set_error(r, "the response is larger than the buffer");
            return false;
        }
        int n = net_tcp_recv(conn, buf + got, cap - got, HTTP_TIMEOUT_MS);
        if (n == 0) break;
        if (n < 0) {
            net_tcp_close(conn);
            set_error(r, got == 0 ? "no response from the server" : "the response timed out");
            return false;
        }
        got += (uint32_t)n;
    }
    net_tcp_close(conn);

    /* The headers end at the first blank line. */
    uint32_t hdr_end = 0;
    for (uint32_t i = 0; i + 3 < got; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            hdr_end = i + 4;
            break;
        }
    }
    if (hdr_end == 0) {
        set_error(r, "the response has no headers");
        return false;
    }

    /* Status line: "HTTP/1.x SSS reason". */
    if (!starts_ci(buf, got, "http/1.")) {
        set_error(r, "the reply is not HTTP");
        return false;
    }
    uint32_t i = 0;
    while (i < got && buf[i] != ' ') i++;
    if (i + 3 >= got || buf[i + 1] < '0' || buf[i + 1] > '9' || buf[i + 2] < '0' || buf[i + 2] > '9' ||
        buf[i + 3] < '0' || buf[i + 3] > '9') {
        set_error(r, "bad status line");
        return false;
    }
    r->status = (buf[i + 1] - '0') * 100 + (buf[i + 2] - '0') * 10 + (buf[i + 3] - '0');

    /* Header lines, one per CRLF, up to the blank line. */
    bool chunked = false, has_len = false;
    uint32_t len = 0;
    uint32_t pos = i;
    while (pos < got && !(buf[pos] == '\r' && pos + 1 < got && buf[pos + 1] == '\n')) pos++;   /* status line end */
    pos += 2;
    while (pos + 2 <= hdr_end) {
        if (buf[pos] == '\r' && buf[pos + 1] == '\n') break;             /* the blank line */
        uint32_t e = pos;
        while (e + 1 < hdr_end && !(buf[e] == '\r' && buf[e + 1] == '\n')) e++;
        read_header(buf + pos, e - pos, r, &chunked, &has_len, &len);
        pos = e + 2;
    }

    uint8_t *body = buf + hdr_end;
    uint32_t avail = got - hdr_end;
    uint32_t body_len = avail;
    if (chunked) {
        if (!decode_chunked(body, avail, &body_len, r)) return false;
    } else if (has_len) {
        if (len > avail) {
            set_error(r, "the connection closed before the whole body arrived");
            return false;
        }
        body_len = len;
    }
    r->body = body;
    r->body_len = body_len;
    return true;
}
