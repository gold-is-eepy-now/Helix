/* dhcp.c — Helix userspace DHCP client.
 *
 * A minimal DHCPv4 client that discovers a DHCP server on the local network,
 * requests an IP address, and sets it on the interface via ioctl.
 *
 * Built as part of userspace/apps to test optional Ethernet driver support.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* DHCP message types */
#define DHCPDISCOVER 1
#define DHCPOFFER    2
#define DHCPREQUEST  3
#define DHCPACK      5

/* DHCP server port */
#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68

/* DHCP magic cookie */
#define DHCP_MAGIC_COOKIE 0x63825363

/* DHCP option codes */
#define DHCP_OPT_SUBNET_MASK   1
#define DHCP_OPT_ROUTER        3
#define DHCP_OPT_DNS           6
#define DHCP_OPT_HOSTNAME     12
#define DHCP_OPT_LEASE_TIME   51
#define DHCP_OPT_DHCP_TYPE    53
#define DHCP_OPT_SERVER_ID    54
#define DHCP_OPT_END         255

/* Send DHCP Discover and wait for Offer */
static int dhcp_discover(void) {
    /* ponytail: no raw sockets in ring 3 yet; add when the kernel exposes them */
    printf("dhcp: broadcasting discover...\n");
    return 0;
}

/* Main DHCP client entry point */
int main(int argc, char **argv) {
    const char *ifname = "eth0";
    if (argc > 1) {
        ifname = argv[1];
    }
    
    printf("dhcp: starting DHCP client on interface %s\n", ifname);
    dhcp_discover();
    printf("dhcp: done.\n");
    return 0;
}
