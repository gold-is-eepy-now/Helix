/* pci.c — PCI configuration space access via the legacy I/O ports.
 *
 * To read a 32-bit config register we write an address word to 0xCF8 and read
 * the data from 0xCFC. The address word packs bus, device, function and the
 * register offset (which must be 4-byte aligned for this mechanism).
 */
#include "pci.h"

#include "io.h"

#define PCI_ADDRESS 0xCF8u
#define PCI_DATA    0xCFCu

static uint32_t address_word(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    return 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)(device & 0x1Fu) << 11) |
           ((uint32_t)(function & 0x07u) << 8) | (offset & 0xFCu);
}

uint32_t pci_config_read32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    outl(PCI_ADDRESS, address_word(bus, device, function, offset));
    return inl(PCI_DATA);
}

void pci_config_write32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset,
                        uint32_t value) {
    outl(PCI_ADDRESS, address_word(bus, device, function, offset));
    outl(PCI_DATA, value);
}

bool pci_find_class(uint8_t class_code, uint8_t subclass, uint8_t prog_if, size_t index,
                    pci_function_t *out) {
    size_t seen = 0;
    for (unsigned bus = 0; bus < 256; bus++) {
        for (unsigned device = 0; device < 32; device++) {
            uint32_t id0 = pci_config_read32((uint8_t)bus, (uint8_t)device, 0, 0x00);
            if ((id0 & 0xFFFFu) == 0xFFFFu) continue;           /* no device here */
            uint32_t header = pci_config_read32((uint8_t)bus, (uint8_t)device, 0, 0x0C);
            unsigned functions = (header & 0x800000u) != 0 ? 8u : 1u;
            for (unsigned function = 0; function < functions; function++) {
                uint32_t vendor = pci_config_read32((uint8_t)bus, (uint8_t)device,
                                                    (uint8_t)function, 0x00);
                if ((vendor & 0xFFFFu) == 0xFFFFu) continue;
                uint32_t klass = pci_config_read32((uint8_t)bus, (uint8_t)device,
                                                   (uint8_t)function, 0x08);
                if (((klass >> 24) & 0xFFu) != class_code) continue;
                if (((klass >> 16) & 0xFFu) != subclass) continue;
                if (((klass >> 8) & 0xFFu) != prog_if) continue;
                if (seen++ != index) continue;
                if (out != NULL) {
                    out->bus = (uint8_t)bus;
                    out->device = (uint8_t)device;
                    out->function = (uint8_t)function;
                    out->prog_if = prog_if;
                    out->vendor_id = (uint16_t)(vendor & 0xFFFFu);
                    out->device_id = (uint16_t)(vendor >> 16);
                }
                return true;
            }
        }
    }
    return false;
}

bool pci_find_device(uint16_t vendor_id, uint16_t device_id, pci_function_t *out) {
    for (unsigned bus = 0; bus < 256; bus++) {
        for (unsigned device = 0; device < 32; device++) {
            uint32_t id0 = pci_config_read32((uint8_t)bus, (uint8_t)device, 0, 0x00);
            if ((id0 & 0xFFFFu) == 0xFFFFu) continue;
            uint32_t header = pci_config_read32((uint8_t)bus, (uint8_t)device, 0, 0x0C);
            unsigned functions = (header & 0x800000u) != 0 ? 8u : 1u;
            for (unsigned function = 0; function < functions; function++) {
                uint32_t id = pci_config_read32((uint8_t)bus, (uint8_t)device, (uint8_t)function, 0x00);
                if ((id & 0xFFFFu) == 0xFFFFu) continue;
                if ((id & 0xFFFFu) != vendor_id || (id >> 16) != device_id) continue;
                if (out != NULL) {
                    out->bus = (uint8_t)bus;
                    out->device = (uint8_t)device;
                    out->function = (uint8_t)function;
                    out->vendor_id = vendor_id;
                    out->device_id = device_id;
                    out->prog_if = (uint8_t)(pci_config_read32((uint8_t)bus, (uint8_t)device,
                                                               (uint8_t)function, 0x08) >> 8);
                }
                return true;
            }
        }
    }
    return false;
}
