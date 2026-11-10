/* net_dhcp.h — DHCP client: get an IPv4 address, mask, gateway and DNS server.
 *
 * The four-message exchange (RFC 2131): DISCOVER (broadcast, from 0.0.0.0), OFFER from a
 * server, REQUEST for the offered address, ACK. Only the ACK changes the configuration.
 * No renewal yet: the lease is taken once, and its time is reported but not counted down. */
#ifndef HELIX_NET_DHCP_H
#define HELIX_NET_DHCP_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t ip[4];
    uint8_t mask[4];
    uint8_t router[4];
    uint8_t dns[4];
    uint8_t server[4];
    uint32_t lease_seconds;
} dhcp_lease_t;

/* Run the exchange within timeout_ms. On success the interface is reconfigured with the
 * lease (net_set_config) and true is returned with the lease in `out`. On failure the
 * configuration is left alone. */
bool net_dhcp_configure(uint32_t timeout_ms, dhcp_lease_t *out);

#endif /* HELIX_NET_DHCP_H */
