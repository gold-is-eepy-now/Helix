/* net_interface.h — network interface object for Helix OS.
 *
 * M12 milestone: networking.
 *
 * Defines a native network interface object. Network interfaces are
 * objects that can be inspected and have basic properties (MAC address,
 * IP configuration, etc.).
 *
 * This is a foundation for a native network stack. Initial implementation
 * provides a simple placeholder for testing the object system.
 */

#ifndef HELIX_NET_INTERFACE_H
#define HELIX_NET_INTERFACE_H

#include "object.h"

typedef struct net_interface net_interface_t;

struct net_interface {
    uint64_t mac_address;
    uint32_t mtu;
    bool active;
    char name[32];
};

/* Initialize a network interface object */
void net_interface_init(net_interface_t *ni, const char *name,
                        uint64_t mac_address, uint32_t mtu);
void net_interface_deinit(net_interface_t *ni);

/* Create a network interface object with the given properties */
object_t *net_interface_create(const char *name, uint64_t mac_address,
                               uint32_t mtu);

#endif /* HELIX_NET_INTERFACE_H */