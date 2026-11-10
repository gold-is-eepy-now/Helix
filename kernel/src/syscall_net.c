/* syscall_net.c — SYS_NET_PING: one echo request from a ring-3 program.
 *
 * The address arrives as one 32-bit value with the first octet in the top byte
 * (10.0.2.2 is 0x0A000202), so no user pointer has to be copied. The reply is
 * matched and timed in the kernel (net_icmp.c); this only checks the arguments. */
#include <stdint.h>

#include "net.h"
#include "net_icmp.h"
#include "syscall.h"

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
