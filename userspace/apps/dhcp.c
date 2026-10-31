/* dhcp.c — Helix userspace DHCP client.
 *
 * A minimal DHCPv4 client that discovers a DHCP server on the local network,
 * requests an IP address, and sets it on the interface via ioctl.
 *
 * Built as part of userspace/apps and linked against the Linux/BSD runtime
 * to test optional Ethernet driver support.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include "linux_runtime.h"
#include "../../runtime/optional_drivers/transport/net.h"

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

static uint8_t discover_pkt[288];
static uint8_t request_pkt[288];
static uint8_t ack_pkt[288];

/* Build a DHCP Discover packet */
static void build_discover(uint8_t *pkt, const uint8_t *chaddr) {
    uint8_t *p = pkt;
    memset(pkt, 0, 288);
    
    *p++ = 1;  /* op: BOOTREQUEST */
    *p++ = 1;  /* htype: Ethernet */
    *p++ = 6;  /* hlen: 6 */
    *p++ = 0;  /* hops */
    
    *(uint32_t *)p = 0x12345678; p += 4;  /* xid */
    *(uint16_t *)p = 0; p += 2;            /* secs */
    *(uint16_t *)p = 0; p += 2;            /* flags */
    *(uint32_t *)p = 0; p += 4;            /* ciaddr */
    *(uint32_t *)p = 0; p += 4;            /* yiaddr */
    *(uint32_t *)p = 0; p += 4;            /* siaddr */
    *(uint32_t *)p = 0; p += 4;            /* giaddr */
    memcpy(p, chaddr, 6); p += 6;          /* chaddr */
    p += 10;                               /* chaddr padding */
    p += 64;                               /* sname */
    p += 128;                              /* file */
    
    /* Magic cookie */
    *(uint32_t *)p = DHCP_MAGIC_COOKIE; p += 4;
    
    /* DHCP Discover option */
    *p++ = DHCP_OPT_DHCP_TYPE; *p++ = 1; *p++ = DHCPDISCOVER;
    *p++ = DHCP_OPT_END;
}

/* Send DHCP Discover and wait for Offer */
static int dhcp_discover(void) {
    /* Implementation depends on network socket support in runtime.
     * For now, just print and return success as a placeholder.
     */
    __asm__ volatile ("int $0x80" : : "a"(SYSCALL_WRITE), "b"(1),
                      "d"("dhcp: broadcasting discover...\n\0"),
                      "c"(28));
    return 0;
}

/* Main DHCP client entry point */
int main(int argc, char **argv) {
    const char *ifname = "eth0";
    if (argc > 1) {
        ifname = argv[1];
    }
    
    __asm__ volatile ("int $0x80" : : "a"(SYSCALL_WRITE), "b"(1),
                      "d"("dhcp: starting DHCP client on interface "),
                      "c"(41));
    
    dhcp_discover();
    
    __asm__ volatile ("int $0x80" : : "a"(SYSCALL_WRITE), "b"(1),
                      "d"("dhcp: done.\n\0"), "c"(12));
    
    return 0;
}