/* e1000.h — Intel 82540EM network driver (the QEMU `e1000` device).
 *
 * Polled, not interrupt driven: callers pull received frames with e1000_receive
 * and push frames with e1000_send. See docs/10-network.md.
 */
#ifndef HELIX_E1000_H
#define HELIX_E1000_H

#include <stdbool.h>
#include <stdint.h>

/* Find the card, reset it, and set up the rings. Returns false if no card is
 * present or it did not come up. Safe to call once at boot. */
bool e1000_init(void);

bool e1000_present(void);
bool e1000_link_up(void);
void e1000_mac(uint8_t out[6]);

/* Send one Ethernet frame (header included, no FCS). False if the frame is too
 * big, the card is absent, or the transmit ring stayed full. */
bool e1000_send(const uint8_t *frame, uint16_t length);

/* Copy the next received frame into `out` (up to `capacity` bytes). Returns its
 * length, or 0 if none is waiting. Frames longer than `capacity` are dropped. */
uint16_t e1000_receive(uint8_t *out, uint16_t capacity);

/* Stage-1 check: send an ARP request for the gateway and wait for QEMU's reply. */
bool e1000_selftest(void);

uint32_t e1000_rx_count(void);
uint32_t e1000_tx_count(void);

#endif /* HELIX_E1000_H */
