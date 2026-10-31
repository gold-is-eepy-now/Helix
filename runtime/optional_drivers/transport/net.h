/* transport/net.h — Helix transport adapter for Linux network drivers.
 *
 * This header provides basic network types shared between Ethernet and WiFi
 * transport adapters.
 */

#ifndef HELIX_TRANSPORT_NET_H
#define HELIX_TRANSPORT_NET_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -- Common network types -------------------------------------------------- */

typedef uint32_t ifindex_t;           /* Network interface index */

#define ETH_ALEN 6                    /* 6 bytes for Ethernet addresses */
#define ETH_HLEN 14                   /* 14 bytes Ethernet header */
#define ETH_TYPE_IP 0x0800            /* IPv4 */
#define ETH_TYPE_ARP 0x0806           /* Address Resolution Protocol */

typedef uint8_t ethaddr[ETH_ALEN];

/* Network packet structure shared between drivers */
typedef struct net_packet_t {
    uint8_t *data;                    /* Packet data */
    uint32_t len;                     /* Packet length */
    uint32_t protocol;                /* ETH_TYPE_* */
    uint32_t ifindex;                 /* Interface index */
    uint32_t timestamp;               /* jiffies or similar */
} net_packet_t;

#ifdef __cplusplus
}
#endif

#endif /* HELIX_TRANSPORT_NET_H */