/* pci.h — PCI configuration space access (legacy mechanism #1: ports 0xCF8/0xCFC).
 *
 * Every USB host controller is a PCI function. Its registers are reached through
 * BARs that PCI config space describes, so the kernel has to be able to read
 * config space before it can talk to USB at all.
 */
#ifndef HELIX_PCI_H
#define HELIX_PCI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint8_t prog_if;
    uint16_t vendor_id;
    uint16_t device_id;
} pci_function_t;

uint32_t pci_config_read32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);
void pci_config_write32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset,
                        uint32_t value);

/* Find the index-th function with the given class / subclass / programming
 * interface, scanning bus 0 to 255. Returns false when there is no such function. */
bool pci_find_class(uint8_t class_code, uint8_t subclass, uint8_t prog_if, size_t index,
                    pci_function_t *out);

/* Find the index-th function with the given vendor / device ID, scanning bus
 * 0 to 255. Returns false when there is no such function. */
bool pci_find_device(uint16_t vendor_id, uint16_t device_id, size_t index,
                     pci_function_t *out);

#endif /* HELIX_PCI_H */
