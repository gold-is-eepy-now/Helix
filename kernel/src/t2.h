/* t2.h — Apple T2 (iBridge) security SoC support.
 *
 * The T2 pairs an A10-derived Secure Enclave Processor with the Box
 * Controller Engine (BCE). The host reaches the SoC over PCIe: the BCE
 * mailbox function owns BAR4 (mailbox registers) and BAR2 (DMA rings).
 *
 * This driver covers the mailbox: locate the function in PCI config space,
 * map its register window, and complete the BCE firmware protocol
 * handshake. SEP services (key wrapping, token storage) sit behind the
 * mailbox and are not implemented yet.
 *
 * Clean-room implementation from freely available protocol descriptions.
 */
#ifndef HELIX_T2_H
#define HELIX_T2_H

#include <stdbool.h>
#include <stdint.h>

#include "object.h"

typedef struct {
    bool present;
    bool firmware_ready;
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint16_t vendor_id;
    uint16_t device_id;
    uint64_t mailbox_phys; /* BAR4: mailbox registers */
    uint64_t dma_phys;     /* BAR2: DMA ring registers */
    uint32_t firmware_version;
    object_t *device_object;
    handle_t control_capability;
} t2_state_t;

/* Register the T2 driver. Must run before driver_init_all(). */
void t2_register(void);

/* Map the mailbox and handshake with the BCE firmware. Must run after
 * vmm_init() because the mailbox lives in MMIO space. */
void t2_start(void);

const t2_state_t *t2_state(void);
void t2_print_status(void);

#endif /* HELIX_T2_H */
