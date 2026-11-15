/* net_dns.h — DNS client: resolve a host name to an IPv4 address (A record).
 *
 * Sends one query per attempt over UDP to the configured server (10.0.2.3 under QEMU,
 * or whatever DHCP gave us), and waits for the answer with the same id. */
#ifndef HELIX_NET_DNS_H
#define HELIX_NET_DNS_H

#include <stdint.h>

#define NET_DNS_OK        0     /* resolved: out holds the address */
#define NET_DNS_NOTFOUND  1     /* the server answered: no such name, or no A record */
#define NET_DNS_NOANSWER  (-1)  /* no answer from the server, or the name is malformed */
#define NET_DNS_SERVFAIL  2     /* the server answered with an error other than "no such name" */

/* The response code (RCODE) of the last answer, for messages: 3 is "no such name". */
int net_dns_last_rcode(void);

int net_dns_resolve(const char *name, uint8_t out[4], uint32_t timeout_ms);

#endif /* HELIX_NET_DNS_H */
