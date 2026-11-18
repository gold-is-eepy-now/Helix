/* net_ethernet.c — builds Ethernet frames around a payload and sends them.
 *
 * The frame buffer is static, not on the stack: a frame is 1.5 KB and the kernel
 * stack is small. Only one frame is built at a time (the stack is single-threaded
 * at this point, and the polled design never sends from two places at once). */
#include "net_ethernet.h"

#include <stddef.h>

#include "e1000.h"
#include "net.h"

bool net_eth_send(const uint8_t dst[6], uint16_t ethertype,
                  const uint8_t *payload, uint16_t length) {
    static uint8_t frame[NET_MAX_FRAME];
    if (length > NET_MAX_FRAME - NET_ETH_HLEN) return false;

    const uint8_t *mac = net_mac();
    for (int i = 0; i < 6; i++) frame[i] = dst[i];
    for (int i = 0; i < 6; i++) frame[6 + i] = mac[i];
    net_put16(frame + 12, ethertype);
    for (uint16_t i = 0; i < length; i++) frame[NET_ETH_HLEN + i] = payload[i];

    return e1000_send(frame, (uint16_t)(NET_ETH_HLEN + length));
}
