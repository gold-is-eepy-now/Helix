/* aml_region.c — the raw byte access behind OperationRegions. Only two address spaces:
 * SystemMemory (through the higher-half direct map, so only memory the boot loader mapped)
 * and SystemIO (x86 port I/O). Other spaces are refused: EC, PCI config and SMBus need their
 * own drivers, which Helix does not have yet. */
#include "aml_internal.h"
#include "io.h"

enum { SPACE_SYSTEM_MEMORY = 0, SPACE_SYSTEM_IO = 1 };

aml_status_t aml_region_read8(uint8_t space, uint64_t addr, uint8_t *out) {
    if (space == SPACE_SYSTEM_MEMORY) {
        volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)(addr + acpi_hhdm());
        *out = *p;
        return AML_OK;
    }
    if (space == SPACE_SYSTEM_IO) {
        if (addr > 0xFFFF) return AML_ERR_HARDWARE;
        *out = inb((uint16_t)addr);
        return AML_OK;
    }
    return AML_ERR_UNSUPPORTED;
}

aml_status_t aml_region_write8(uint8_t space, uint64_t addr, uint8_t value) {
    if (space == SPACE_SYSTEM_MEMORY) {
        volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)(addr + acpi_hhdm());
        *p = value;
        return AML_OK;
    }
    if (space == SPACE_SYSTEM_IO) {
        if (addr > 0xFFFF) return AML_ERR_HARDWARE;
        outb((uint16_t)addr, value);
        return AML_OK;
    }
    return AML_ERR_UNSUPPORTED;
}
