/* acpi_spi_res.h — read an SPI connection from an ACPI _CRS resource buffer.
 *
 * A device on an SPI bus describes its wiring in _CRS with a "SpiSerialBus" resource
 * (ACPI 6.x, section 6.4.3.8.2). This parser reads that one descriptor. It has no kernel
 * dependencies, so tests/acpi-spi-res-unit.c checks it on the host.
 *
 * Layout (offsets from the start of the descriptor, all little-endian):
 *   0      0x8E  large item, serial bus connection
 *   1-2    length of the rest of the descriptor
 *   3      revision (1)              4  resource source index
 *   5      bus type (2 = SPI)        6  general flags
 *   7-8    type-specific flags: bit 0 wire mode (1 = 3-wire), bit 1 chip-select polarity
 *   9      type-specific revision    10-11 type data length (at least 9)
 *   12-15  connection speed, Hz
 *   16     data bit length           17 clock phase    18 clock polarity
 *   19-20  device selection (the chip-select number)
 * A resource source string may follow; it is not needed here. */
#ifndef HELIX_ACPI_SPI_RES_H
#define HELIX_ACPI_SPI_RES_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t speed_hz;
    uint8_t data_bit_length;
    uint8_t clock_phase;      /* 0: sample on the first edge, 1: on the second */
    uint8_t clock_polarity;   /* 0: clock idle low, 1: clock idle high */
    uint16_t chip_select;     /* device selection */
    bool three_wire;          /* wire mode 1: MOSI and MISO share one line */
    bool cs_active_high;      /* chip-select polarity */
} acpi_spi_res_t;

typedef enum {
    ACPI_SPI_RES_OK = 0,
    ACPI_SPI_RES_NONE,        /* no SpiSerialBus descriptor in the buffer */
    ACPI_SPI_RES_TRUNCATED,   /* a descriptor runs past the end of the buffer */
    ACPI_SPI_RES_BAD,         /* a descriptor is too short or has an unknown revision */
} acpi_spi_res_status_t;

/* Finds the first SpiSerialBus descriptor in a _CRS buffer and reads it. Other resources
 * (I2C, GPIO, memory, end tag) are skipped. */
acpi_spi_res_status_t acpi_spi_res_parse(const uint8_t *buf, uint32_t len, acpi_spi_res_t *out);

#endif /* HELIX_ACPI_SPI_RES_H */
