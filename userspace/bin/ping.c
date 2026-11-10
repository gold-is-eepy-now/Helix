/* ping.c: /bin/ping <a.b.c.d> — four ICMP echo requests through the kernel's
 * network stack (SYS_NET_PING). The kernel does the sending, the matching and the
 * timing; this program parses the address and prints the results. */
#include <stdio.h>
#include "libhelix.h"

/* Parse "a.b.c.d" into a big-endian u32 (first octet in the top byte). 0 on failure. */
static int parse_ipv4(const char *s, uint32_t *out) {
    uint32_t ip = 0;
    int part = 0, value = 0, digits = 0;
    for (const char *p = s;; p++) {
        if (*p >= '0' && *p <= '9') {
            value = value * 10 + (*p - '0');
            digits++;
            if (value > 255 || digits > 3) return 0;
        } else if (*p == '.' || *p == '\0') {
            if (digits == 0 || part > 3) return 0;
            ip = (ip << 8) | (uint32_t)value;
            part++;
            value = 0;
            digits = 0;
            if (*p == '\0') break;
        } else {
            return 0;
        }
    }
    if (part != 4) return 0;
    *out = ip;
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: ping <a.b.c.d>\n");
        return 1;
    }
    uint32_t ip;
    if (!parse_ipv4(argv[1], &ip)) {
        printf("ping: not an IPv4 address: %s\n", argv[1]);
        return 1;
    }
    printf("PING %s: 32 data bytes\n", argv[1]);
    int received = 0;
    for (uint16_t seq = 1; seq <= 4; seq++) {
        int64_t us = helix_net_ping(ip, seq, 1000);
        if (us >= 0) {
            received++;
            printf("reply from %s: seq=%u time=%u us\n", argv[1], (unsigned)seq, (unsigned)us);
        } else {
            printf("request timed out: seq=%u\n", (unsigned)seq);
        }
    }
    printf("4 sent, %d received\n", received);
    return received > 0 ? 0 : 1;
}
