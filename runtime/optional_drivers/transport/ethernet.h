/* transport/ethernet.h — Helix transport adapter for Linux Ethernet drivers.
 *
 * This header provides the minimal kernel ABI for Linux's "net_device" structure
 * and core operations (netif_*). The real driver code in the Linux kernel
 * is compiled with this header instead of the original linux/netdevice.h
 * so that it is aware of Helix's transport layer.
 *
 * Because these are *stub* adapters, the actual driver logic is either
 * simplified (just enough to satisfy the Linux driver model) or
 * forwarded to the Helix native driver framework via the transport
 * context. The user may later replace this stub with a proper Linux
 * driver emulation layer.
 *
 * Includes transport/net.h for common network types.
 */

#ifndef HELIX_TRANSPORT_ETHERNET_H
#define HELIX_TRANSPORT_ETHERNET_H

#include <stdint.h>
#include <stddef.h>
#include "net.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -- Basic network types -------------------------------------------------- */
#define ETH_ALEN 6                /* 6 bytes for Ethernet addresses */
#define ETH_HLEN 14               /* 14 bytes Ethernet header */
#define ETH_TYPE_IP 0x0800        /* IPv4 */
#define ETH_TYPE_ARP 0x0806       /* Address Resolution Protocol */

typedef uint8_t ethaddr[ETH_ALEN];

/* -- Linux net_device stub type ------------------------------------------- */

/* Use the net_packet_t from transport/net.h */

/* Linux driver ops stub (subset of struct net_device_ops) */
typedef struct net_device_ops {
    int (*ndo_open)(void *dev);
    int (*ndo_stop)(void *dev);
    int (*ndo_start_xmit)(void *dev, const uint8_t *buf, uint32_t len);
    int (*ndo_set_mac_address)(void *dev, const uint8_t *addr);
    int (*ndo_do_ioctl)(void *dev, int cmd, void *data);
} net_device_ops_t;

/* Minimal packet buffer stub (matches Linux's sk_buff structure) */
typedef struct sk_buff {
    uint8_t *head;                /* Head of packet data */
    uint32_t len;                 /* Length of packet */
    void *dev;                    /* Device that received it */
    uint8_t protocol;             /* Protocol (ETH_TYPE_* constants) */
    void *priv;                   /* Private data */
} sk_buff_t;

/* -- Transport adapter functions ------------------------------------------- */

/* Allocate and initialize a net_device stub */
void *transport_netdev_alloc(const char *name, ifindex_t ifindex);

/* Register a net_device with the transport context */
int transport_netdev_register(void *dev);

/* Free a net_device stub */
void transport_netdev_free(void *dev);

/* Basic device operations */
int netif_rx(void *skb);    /* Receive packet */
int netif_transmit(void *skb); /* Transmit packet */

#ifdef __cplusplus
}
#endif

#endif /* HELIX_TRANSPORT_ETHERNET_H */