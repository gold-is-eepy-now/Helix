/* intel_lpss_gspi.h — Intel LPSS General-Purpose SPI controller (PCI 8086:9CE6, "SPI1"
 * on Broadwell-U, the controller the MacBook Air 7,2 uses for its internal SPI devices).
 *
 * This is the Intel serial-IO SPI block, not the PCH firmware-flash interface. */
#ifndef HELIX_INTEL_LPSS_GSPI_H
#define HELIX_INTEL_LPSS_GSPI_H

#include <stdbool.h>
#include <stdint.h>

#define INTEL_LPSS_GSPI_VENDOR 0x8086u
#define INTEL_LPSS_GSPI_DEVICE 0x9CE6u

typedef struct {
    bool found;            /* the PCI function exists */
    bool ready;            /* initialisation verified the registers; transfers allowed */
    uint8_t bus, device, function;
    uint64_t bar_phys;     /* memory BAR0 base, from PCI config space */
    uint32_t sscr0_readback;   /* SSCR0 as read back during init, to confirm the mapping */
    uint32_t sssr;             /* status register at init */
    uint8_t scr;               /* serial clock rate field in use (see the driver for the formula) */
} intel_lpss_gspi_info_t;

/* Find the controller, map its registers, reset it and verify them. Returns true when the
 * controller is ready. Does nothing harmful when the device is absent (QEMU, for example). */
bool intel_lpss_gspi_init(void);

void intel_lpss_gspi_info(intel_lpss_gspi_info_t *out);

#endif /* HELIX_INTEL_LPSS_GSPI_H */
