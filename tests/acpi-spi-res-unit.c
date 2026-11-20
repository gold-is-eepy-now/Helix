/* acpi-spi-res-unit.c — host tests for kernel/src/acpi_spi_res.c.
 *
 * The buffers are built byte by byte from the layout in acpi_spi_res.h, not by calling the
 * parser's own helpers, so a wrong offset in the parser shows up here. Build and run:
 *   cc -std=gnu11 -Wall -Wextra -Ikernel/src -o build/test/acpi-spi-res-unit \
 *      tests/acpi-spi-res-unit.c kernel/src/acpi_spi_res.c && build/test/acpi-spi-res-unit
 */
#include <stdio.h>
#include <string.h>
#include "acpi_spi_res.h"

static int g_fail, g_pass;

static void check(int ok, const char *what) {
    printf("acpi-spi-res-unit: %s %s\n", ok ? "ok  " : "FAIL", what);
    if (ok) g_pass++; else g_fail++;
}

/* A SpiSerialBus descriptor, 21 bytes, followed by the end tag. Length 18 is the number of
 * bytes after the 3-byte header. Speed 1,000,000 Hz (40 42 0F 00), 8 bits, phase 1, polarity 1,
 * chip select 3. */
static const uint8_t SPI_DESC[] = {
    0x8E, 0x12, 0x00,             /* large item 0x0E, length 18 */
    0x01, 0x00, 0x02, 0x00,       /* revision 1, source index 0, bus type 2 (SPI), flags 0 */
    0x00, 0x00,                   /* type-specific flags: 4-wire, active-low chip select */
    0x01, 0x09, 0x00,             /* type revision 1, type data length 9 */
    0x40, 0x42, 0x0F, 0x00,       /* connection speed 1000000 */
    0x08, 0x01, 0x01,             /* data bits 8, clock phase 1, clock polarity 1 */
    0x03, 0x00,                   /* device selection 3 */
    0x79, 0x00                    /* end tag */
};

/* An I2C connection (bus type 1): 3 header bytes plus 18 more, 21 bytes. No end tag here:
 * a _CRS buffer has one end tag, after its last descriptor, and the walker stops at the first. */
static const uint8_t I2C_DESC[] = {
    0x8E, 0x12, 0x00,
    0x01, 0x00, 0x01, 0x00,       /* bus type 1: I2C */
    0x00, 0x00, 0x01, 0x06, 0x00,
    0x00, 0x00, 0x00, 0x00,       /* speed field, unused for I2C here */
    0x00, 0x00, 0x00, 0x00, 0x00,
};

/* A small item (name 0x04, 7 data bytes) that the walker must skip. Tag = (4 << 3) | 7 = 0x27. */
static const uint8_t SMALL_SKIP[] = { 0x27, 1, 2, 3, 4, 5, 6, 7 };

int main(void) {
    acpi_spi_res_t r;
    memset(&r, 0, sizeof r);
    acpi_spi_res_status_t st = acpi_spi_res_parse(SPI_DESC, sizeof SPI_DESC, &r);
    check(st == ACPI_SPI_RES_OK, "a SpiSerialBus descriptor parses");
    check(r.speed_hz == 1000000u, "connection speed is 1,000,000 Hz");
    check(r.data_bit_length == 8u, "data bit length is 8");
    check(r.clock_phase == 1u && r.clock_polarity == 1u, "clock phase and polarity are read");
    check(r.chip_select == 3u, "device selection is the chip select");
    check(!r.three_wire && !r.cs_active_high, "4-wire and active-low flags are clear");

    /* Flags: bit 0 set means 3-wire, bit 1 set means active-high chip select. */
    uint8_t flagged[sizeof SPI_DESC];
    memcpy(flagged, SPI_DESC, sizeof SPI_DESC);
    flagged[7] = 0x03;
    memset(&r, 0, sizeof r);
    st = acpi_spi_res_parse(flagged, sizeof flagged, &r);
    check(st == ACPI_SPI_RES_OK && r.three_wire && r.cs_active_high,
          "type-specific flags bit 0 gives 3-wire, bit 1 gives active-high");

    /* An I2C descriptor first: it is skipped, then the SPI one is found. */
    uint8_t mixed[sizeof I2C_DESC + sizeof SPI_DESC];
    memcpy(mixed, I2C_DESC, sizeof I2C_DESC);
    memcpy(mixed + sizeof I2C_DESC, SPI_DESC, sizeof SPI_DESC);
    memset(&r, 0, sizeof r);
    st = acpi_spi_res_parse(mixed, sizeof mixed, &r);
    check(st == ACPI_SPI_RES_OK && r.chip_select == 3u, "an I2C descriptor is skipped to find the SPI one");

    /* A small item is skipped by its length. */
    uint8_t small[sizeof SMALL_SKIP + sizeof SPI_DESC];
    memcpy(small, SMALL_SKIP, sizeof SMALL_SKIP);
    memcpy(small + sizeof SMALL_SKIP, SPI_DESC, sizeof SPI_DESC);
    memset(&r, 0, sizeof r);
    st = acpi_spi_res_parse(small, sizeof small, &r);
    check(st == ACPI_SPI_RES_OK && r.chip_select == 3u, "a small item is skipped by its length");

    /* A buffer with only the end tag has no SPI descriptor. */
    static const uint8_t ONLY_END[] = { 0x79, 0x00 };
    st = acpi_spi_res_parse(ONLY_END, sizeof ONLY_END, &r);
    check(st == ACPI_SPI_RES_NONE, "a buffer with only an end tag reports none");
    st = acpi_spi_res_parse(NULL, 0, &r);
    check(st == ACPI_SPI_RES_BAD, "a NULL buffer is refused");

    /* Cut in the middle of the descriptor: the walker must not read past the end. */
    st = acpi_spi_res_parse(SPI_DESC, 10, &r);
    check(st == ACPI_SPI_RES_TRUNCATED, "a descriptor cut short is reported as truncated");

    /* Revision 2 is not a revision this parser knows. */
    uint8_t rev2[sizeof SPI_DESC];
    memcpy(rev2, SPI_DESC, sizeof SPI_DESC);
    rev2[3] = 0x02;
    st = acpi_spi_res_parse(rev2, sizeof rev2, &r);
    check(st == ACPI_SPI_RES_BAD, "an unknown revision is refused");

    /* Type data length below 9 cannot hold the fields, so it is refused. */
    uint8_t short_type[sizeof SPI_DESC];
    memcpy(short_type, SPI_DESC, sizeof SPI_DESC);
    short_type[10] = 0x08;
    st = acpi_spi_res_parse(short_type, sizeof short_type, &r);
    check(st == ACPI_SPI_RES_BAD, "a type data length under 9 is refused");

    printf("acpi-spi-res-unit: %s (%d passed, %d failed)\n", g_fail ? "FAILED" : "PASS", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
