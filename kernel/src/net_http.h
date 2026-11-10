/* net_http.h — an HTTP/1.1 client over net_tcp (plain http:// only; see docs/10-network.md).
 *
 * One request per connection, with "Connection: close", so the response ends at the
 * close, at Content-Length, or at the last chunk of chunked encoding. Redirects are not
 * followed here: the caller reads `location` and asks again. */
#ifndef HELIX_NET_HTTP_H
#define HELIX_NET_HTTP_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int status;              /* the status code, e.g. 200, 301, 404 */
    char location[256];      /* the Location header, or "" */
    char content_type[64];   /* the Content-Type header, or "" */
    uint8_t *body;           /* points into the caller's buffer */
    uint32_t body_len;
    char error[96];          /* why the request failed (set when net_http_get returns false) */
} net_http_result_t;

/* GET an http:// URL. The raw response is read into buf (up to cap bytes), and the body
 * is decoded in place there. True when a status line arrived, for any status; false,
 * with r->error set, otherwise. Blocks, polling the network, for a few seconds per step. */
bool net_http_get(const char *url, uint8_t *buf, uint32_t cap, net_http_result_t *r);

#endif /* HELIX_NET_HTTP_H */
