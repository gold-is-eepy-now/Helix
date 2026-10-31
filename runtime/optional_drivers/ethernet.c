/* ethernet.c — Helix transport adapter for Linux Ethernet drivers.
 *
 * This file provides the minimal translation layer that allows Linux's
 * Ethernet driver model to work with Helix's native object framework.
 *
 * It implements a "stub" that adapts the Linux net_device structure to
 * Helix's driver framework. The actual driver logic is expected to be
 * compiled with this header, allowing drivers like r815x, e1000, etc.
 * to be built as optional components.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

/* -- Local types for the transport layer ---------------------------------- */

typedef uint32_t ifindex_t;

typedef struct {
    char name[16];
    uint32_t ifindex;
    uint32_t mtu;
    uint32_t flags;
    uint32_t state;
    uint32_t rx_packets;
    uint32_t tx_packets;
    uint32_t rx_bytes;
    uint32_t tx_bytes;
    void *priv;
    void *ops;
    int (*init)(void *dev);
    int (*uninit)(void *dev);
    int (*transmit)(void *dev, const uint8_t *buf, uint32_t len);
} net_device_t;

typedef struct {
    uint8_t *data;
    uint32_t len;
    uint32_t protocol;
    uint32_t ifindex;
    uint32_t timestamp;
} net_packet_t;

/* -- Ethernet device state ------------------------------------------------- */

typedef struct {
    net_device_t *netdev;
    uint32_t state;
    uint32_t rx_packets;
    uint32_t tx_packets;
    uint32_t rx_bytes;
    uint32_t tx_bytes;
} ethernet_dev_t;

/* -- Transport adapter functions ------------------------------------------- */

/* Allocate a network device */
net_device_t *transport_netdev_alloc(const char *name, ifindex_t ifindex) {
    ethernet_dev_t *dev = (ethernet_dev_t *)malloc(sizeof(ethernet_dev_t));
    if (!dev) return NULL;
    
    dev->netdev = (net_device_t *)malloc(sizeof(net_device_t));
    if (!dev->netdev) {
        free(dev);
        return NULL;
    }
    
    strncpy(dev->netdev->name, name, sizeof(dev->netdev->name) - 1);
    dev->netdev->name[sizeof(dev->netdev->name) - 1] = '\0';
    dev->netdev->ifindex = ifindex;
    dev->netdev->mtu = 1500;
    dev->netdev->state = 0;
    dev->netdev->flags = 0;
    dev->netdev->rx_packets = 0;
    dev->netdev->tx_packets = 0;
    dev->netdev->rx_bytes = 0;
    dev->netdev->tx_bytes = 0;
    dev->netdev->priv = dev;
    dev->netdev->ops = NULL;
    dev->netdev->init = NULL;
    dev->netdev->uninit = NULL;
    dev->netdev->transmit = NULL;
    
    return dev->netdev;
}

/* Register a net_device with the transport context */
int transport_netdev_register(net_device_t *dev) {
    return 0;
}

/* Free a network device */
void transport_netdev_free(net_device_t *dev) {
    if (dev) {
        free(dev->priv);
        free(dev);
    }
    return;
}

/* Send a packet */
int transport_netdev_transmit(net_device_t *dev, const net_packet_t *pkt) {
    if (dev && pkt) {
        return 0;
    }
    return -1;
}