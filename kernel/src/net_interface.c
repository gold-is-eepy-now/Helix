/* net_interface.c — network interface object implementation for Helix OS.
 *
 * M12 milestone: networking.
 *
 * Provides a simple network interface object that can be created and
 * inspected. The actual network stack is not implemented yet, but the
 * object system allows for network interface objects to be created and
 * managed through the object manager.
 */

#include "net_interface.h"
#include "console.h"
#include "heap.h"
#include "kstring.h"

#define MAX_NET_INTERFACES 8

static net_interface_t *g_net_interfaces[MAX_NET_INTERFACES];
static uint8_t g_net_interface_count;

void net_interface_init(net_interface_t *ni, const char *name,
                        uint64_t mac_address, uint32_t mtu) {
    strcpy(ni->name, name);
    if (strlen(name) < 31) strcpy(ni->name, name);
    ni->mac_address = mac_address;
    ni->mtu = mtu;
    ni->active = false;
}

void net_interface_deinit(net_interface_t *ni) {
    if (ni) {
        for (uint8_t i = 0; i < g_net_interface_count; i++) {
            if (g_net_interfaces[i] == ni) {
                g_net_interfaces[i] = NULL;
                g_net_interface_count--;
                kfree(ni);
                return;
            }
        }
    }
}

object_t *net_interface_create(const char *name, uint64_t mac_address,
                               uint32_t mtu) {
    if (g_net_interface_count >= MAX_NET_INTERFACES) return NULL;

    net_interface_t *ni = kzalloc(sizeof(*ni));
    if (ni == NULL) return NULL;

    strcpy(ni->name, name);
    if (strlen(name) < 31) strcpy(ni->name, name);
    ni->mac_address = mac_address;
    ni->mtu = mtu;
    ni->active = false;

    g_net_interfaces[g_net_interface_count++] = ni;

    /* Create an object for this interface */
    object_t *obj = object_create(OBJECT_TYPE_DEVICE, sizeof(*ni), NULL);
    if (obj == NULL) {
        g_net_interface_count--;
        kfree(ni);
        return NULL;
    }
    memcpy(object_payload(obj), ni, sizeof(*ni));
    return obj;
}