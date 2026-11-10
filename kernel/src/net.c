/* net.c — interface state and the receive dispatch for the kernel network stack.
 *
 * The address is fixed. QEMU's user-mode network always gives a guest 10.0.2.15 behind
 * a gateway at 10.0.2.2, so there is no DHCP yet (docs/10-network.md stage 7). */
#include "net.h"

#include <stddef.h>

#include "e1000.h"
#include "klog.h"
#include "net_arp.h"
#include "net_ethernet.h"
#include "net_ip.h"

static uint8_t g_mac[6];
static uint8_t g_ip[4]      = {10, 0, 2, 15};
static uint8_t g_gateway[4] = {10, 0, 2, 2};
static uint8_t g_netmask[4] = {255, 255, 255, 0};
static bool g_ready;

void net_init(void) {
    if (!e1000_present()) return;
    e1000_mac(g_mac);
    g_ready = true;
    klog_note(KLOG_INPUT, "[net] stack up: %u.%u.%u.%u/24, gateway %u.%u.%u.%u\n",
              g_ip[0], g_ip[1], g_ip[2], g_ip[3], g_gateway[0], g_gateway[1],
              g_gateway[2], g_gateway[3]);
}

bool net_ready(void) { return g_ready; }
const uint8_t *net_mac(void) { return g_mac; }
const uint8_t *net_ip_addr(void) { return g_ip; }
const uint8_t *net_gateway(void) { return g_gateway; }
const uint8_t *net_netmask(void) { return g_netmask; }

void net_poll(void) {
    /* On the stack, not static: an echo reply can send, which polls again and
     * would overwrite a shared buffer while this frame is still being read. */
    uint8_t frame[NET_MAX_FRAME];
    /* A cap keeps one busy segment from holding the caller forever. */
    for (int i = 0; i < 64; i++) {
        uint16_t n = e1000_receive(frame, NET_MAX_FRAME);
        if (n == 0) break;
        if (n < NET_ETH_HLEN) continue;          /* runt: not a frame we can read */
        uint16_t type = net_get16(frame + 12);
        if (type == NET_ETHERTYPE_ARP) {
            net_arp_input(frame, n);
        } else if (type == NET_ETHERTYPE_IPV4) {
            net_ip_input(frame, n);
        }                                        /* anything else: ignored */
    }
}

bool net_parse_ip(const char *text, uint8_t out[4]) {
    int part = 0;
    int value = 0;
    int digits = 0;
    for (const char *p = text;; p++) {
        if (*p >= '0' && *p <= '9') {
            value = value * 10 + (*p - '0');
            digits++;
            if (value > 255 || digits > 3) return false;
        } else if (*p == '.' || *p == '\0') {
            if (digits == 0 || part > 3) return false;
            out[part++] = (uint8_t)value;
            value = 0;
            digits = 0;
            if (*p == '\0') break;
        } else {
            return false;
        }
    }
    return part == 4;
}
