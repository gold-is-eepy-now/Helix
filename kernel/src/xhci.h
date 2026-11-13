/* xhci.h — xHCI (USB 3) host controller, stage 1: read-only discovery.
 *
 * Finds the controller by PCI class (0C/03/30), maps its registers, decodes the capability
 * registers and reads each port's status. It writes nothing to the controller. Reset, rings,
 * slots and transfers come in later stages (see docs/19-xhci.md). */
#ifndef HELIX_XHCI_H
#define HELIX_XHCI_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool found;
    uint8_t bus, device, function;
    uint64_t bar_phys;
    uint16_t hci_version;      /* from the capability registers, e.g. 0x0100 for xHCI 1.0 */
    uint8_t cap_length;        /* offset from the base to the operational registers */
    uint8_t max_ports;         /* HCSPARAMS1 MaxPorts */
    uint16_t max_slots;        /* HCSPARAMS1 MaxSlots */
    uint16_t max_intrs;        /* HCSPARAMS1 MaxIntrs */
    uint32_t hcsparams1;       /* raw registers, for the boot log */
    uint32_t hccparams1;
    uint32_t usbsts;           /* USBSTS as read: halted bit, errors */
    uint32_t pagesize;         /* the PAGESIZE register, raw */
    uint8_t ports_connected;   /* ports with a device present (PORTSC bit 0) */
    uint8_t ports_powered;     /* ports with power on (PORTSC bit 9) */
} xhci_info_t;

/* Find and read the controller. Returns true when it was found and its registers read back.
 * Does nothing if there is no xHCI controller (QEMU without -device qemu-xhci, the iMac). */
bool xhci_init(void);
void xhci_get_info(xhci_info_t *out);

#endif /* HELIX_XHCI_H */
