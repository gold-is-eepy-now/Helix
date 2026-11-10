/* nslookup.c: /bin/nslookup <name> — resolve a host name through the kernel's DNS
 * client (SYS_NET_RESOLVE). The kernel sends the query to the configured server. */
#include <stdio.h>
#include "libhelix.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: nslookup <name>\n");
        return 1;
    }
    int64_t r = helix_net_resolve(argv[1]);
    if (r == -1) {
        printf("nslookup: %s: not found\n", argv[1]);
        return 1;
    }
    if (r < 0) {
        printf("nslookup: no answer from the DNS server\n");
        return 1;
    }
    uint32_t ip = (uint32_t)r;
    printf("%s is %u.%u.%u.%u\n", argv[1],
           (unsigned)(ip >> 24) & 255u, (unsigned)(ip >> 16) & 255u,
           (unsigned)(ip >> 8) & 255u, (unsigned)ip & 255u);
    return 0;
}
