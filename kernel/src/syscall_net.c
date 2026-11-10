/* syscall_net.c — SYS_NET_PING: one echo request from a ring-3 program.
 *
 * The address arrives as one 32-bit value with the first octet in the top byte
 * (10.0.2.2 is 0x0A000202), so no user pointer has to be copied. The reply is
 * matched and timed in the kernel (net_icmp.c); this only checks the arguments. */
#include <stdint.h>

#include "net.h"
#include "net_dns.h"
#include "net_icmp.h"
#include "syscall.h"
#include "usermem.h"

#define PING_TIMEOUT_MAX_MS 10000u

uint64_t sys_net_ping(uint64_t ip_be, uint64_t seq, uint64_t timeout_ms,
                      uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a4; (void)a5; (void)a6; (void)a7;
    if (!net_ready()) return UINT64_MAX;
    if (timeout_ms == 0 || timeout_ms > PING_TIMEOUT_MAX_MS) return UINT64_MAX;
    uint8_t dst[4] = {
        (uint8_t)(ip_be >> 24), (uint8_t)(ip_be >> 16), (uint8_t)(ip_be >> 8), (uint8_t)ip_be,
    };
    uint32_t rtt_us = 0;
    if (!net_ping(dst, (uint16_t)seq, (uint32_t)timeout_ms, &rtt_us)) return UINT64_MAX;
    return rtt_us;
}

#define NAME_MAX_USER   256u
#define RESOLVE_TIMEOUT_MS 3000u

/* SYS_NET_RESOLVE(name): the name is a NUL-terminated string in user memory. The answer
 * is packed like the ping address (first octet in the top byte). -1 means the server said
 * no such name; -2 means it did not answer. */
uint64_t sys_net_resolve(uint64_t name_addr, uint64_t a2, uint64_t a3,
                         uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    char name[NAME_MAX_USER];
    if (!usermem_copy_str(name, sizeof name, name_addr)) return (uint64_t)(int64_t)-2;
    uint8_t ip[4];
    int r = net_dns_resolve(name, ip, RESOLVE_TIMEOUT_MS);
    if (r == NET_DNS_NOTFOUND) return (uint64_t)(int64_t)-1;
    if (r != NET_DNS_OK) return (uint64_t)(int64_t)-2;     /* no answer, or a server error */
    return ((uint64_t)ip[0] << 24) | ((uint64_t)ip[1] << 16) | ((uint64_t)ip[2] << 8) | ip[3];
}
