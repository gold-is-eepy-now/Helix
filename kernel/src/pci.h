/* pci.h — PCI bus 0 inventory, used to locate USB host controllers. */
#ifndef HELIX_PCI_H
#define HELIX_PCI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint8_t programming_interface;
    uint16_t vendor_id;
    uint16_t device_id;
    uint32_t bar0;
    uint64_t bar0_phys;
    uint64_t xhci_mmio_virt;
    uint16_t xhci_version;
    uint8_t xhci_capability_length;
    uint8_t xhci_max_slots;
    uint8_t xhci_max_ports;
    bool xhci_capabilities_read;
    uint64_t ehci_mmio_virt;
    uint8_t ehci_capability_length;
    uint8_t ehci_max_ports;
    bool ehci_port_power_control;
    bool ehci_capabilities_read;
} pci_usb_controller_t;

typedef struct {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint16_t vendor_id;
    uint16_t device_id;
    uint32_t bar5;
    uint64_t abar_phys;
    uint64_t abar_virt;
} pci_ahci_controller_t;

void pci_init(uint64_t hhdm_offset);
size_t pci_usb_controller_count(void);
bool pci_usb_controller_get(size_t index, pci_usb_controller_t *controller);
void pci_print_usb_controllers(void);
size_t pci_ahci_controller_count(void);
bool pci_ahci_controller_get(size_t index, pci_ahci_controller_t *controller);
void pci_print_ahci_controllers(void);

#endif /* HELIX_PCI_H */
