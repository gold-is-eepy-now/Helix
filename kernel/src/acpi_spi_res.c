/* acpi_spi_res.c — reads the first SpiSerialBus descriptor from a _CRS buffer.
 *
 * Resource descriptors come in two forms (ACPI 6.x, section 6.4.2):
 *   small: bit 7 clear; bits 2:0 give the length of the data; bits 6:3 the item name.
 *   large: bit 7 set;   byte 0 is the item name; bytes 1-2 give the length of the data.
 * Walking the buffer needs both forms, so an unknown item can be skipped by its length. */
#include <stddef.h>   /* NULL */
#include "acpi_spi_res.h"

#define ITEM_END_TAG_SMALL  0x0Fu   /* small item name 0xF: end of the template */
#define ITEM_SERIAL_BUS     0x0Eu   /* large item name 0xE: serial bus connection */
#define SERIAL_TYPE_SPI     2u
#define SERIAL_REV_SPI      1u
#define SPI_MIN_TYPE_DATA   9u      /* type data length: speed, bits, phase, polarity, selection */
#define SPI_FIXED_BYTES     21u     /* descriptor bytes up to and including device selection */

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

acpi_spi_res_status_t acpi_spi_res_parse(const uint8_t *buf, uint32_t len, acpi_spi_res_t *out) {
    if (buf == NULL || out == NULL) return ACPI_SPI_RES_BAD;
    uint32_t pos = 0;
    while (pos < len) {
        uint8_t tag = buf[pos];
        if ((tag & 0x80u) == 0) {
            /* Small item: the end tag stops the walk; anything else is skipped by its length. */
            uint32_t data = tag & 0x07u;
            if (((tag >> 3) & 0x0Fu) == ITEM_END_TAG_SMALL) break;
            if (pos + 1 + data > len) return ACPI_SPI_RES_TRUNCATED;
            pos += 1 + data;
            continue;
        }
        /* Large item: 3 header bytes, then length bytes of data. */
        if (pos + 3 > len) return ACPI_SPI_RES_TRUNCATED;
        uint32_t data = rd16(&buf[pos + 1]);
        if (pos + 3 + data > len) return ACPI_SPI_RES_TRUNCATED;
        if ((tag & 0x7Fu) == ITEM_SERIAL_BUS) {
            const uint8_t *d = &buf[pos];
            /* Only the SPI form is read here. An I2C or UART connection is skipped. */
            if (d[5] != SERIAL_TYPE_SPI) { pos += 3 + data; continue; }
            if (data + 3 < SPI_FIXED_BYTES) return ACPI_SPI_RES_BAD;
            if (d[3] != SERIAL_REV_SPI) return ACPI_SPI_RES_BAD;
            if (rd16(&d[10]) < SPI_MIN_TYPE_DATA) return ACPI_SPI_RES_BAD;
            uint16_t flags = rd16(&d[7]);
            out->three_wire = (flags & 0x1u) != 0;
            out->cs_active_high = (flags & 0x2u) != 0;
            out->speed_hz = rd32(&d[12]);
            out->data_bit_length = d[16];
            out->clock_phase = d[17];
            out->clock_polarity = d[18];
            out->chip_select = rd16(&d[19]);
            return ACPI_SPI_RES_OK;
        }
        pos += 3 + data;
    }
    return ACPI_SPI_RES_NONE;
}
