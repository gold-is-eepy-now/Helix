/* apple_topcase_spi.c — read-only probe of the Apple top-case device.
 *
 * Settings come from the macOS registry dump of SPIT@0 (APP000D): mode 0 (spiSPO 0,
 * spiSPH 0), 8-bit words. Frames are 256 bytes; the last two are a CRC-16 over the rest,
 * the kernel's crc16() (CRC-16/ARC, init 0, reflected 0xA001). The CRC variant is taken from
 * the Linux driver and not yet checked against a second source; a match on a real frame
 * would confirm it.
 *
 * This probe writes nothing: it sends zeros on MOSI and keeps the bytes that come back. */
#include "apple_topcase_spi.h"

#include <stddef.h>

#include "spi.h"

static uint16_t crc16_arc(const uint8_t *p, uint32_t len) {
    uint16_t crc = 0;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (unsigned b = 0; b < 8; b++) {
            crc = (crc & 1u) ? (uint16_t)((crc >> 1) ^ 0xA001u) : (uint16_t)(crc >> 1);
        }
    }
    return crc;
}

static uint8_t frame[TOPCASE_FRAME_SIZE];

int topcase_probe(topcase_probe_t *out) {
    for (size_t i = 0; i < sizeof *out; i++) ((uint8_t *)out)[i] = 0;

    spi_controller_t *c = spi_controller_at(0);
    if (c == NULL || !c->ready) {
        out->status = SPI_ERR_NOT_READY;
        return out->status;
    }
    spi_device_t dev = {
        .ctrl = c,
        .chip_select = 0,
        .mode = 0,            /* spiSPO 0, spiSPH 0 */
        .bits = 8,
        .speed_hz = 8000000,  /* spiSclkPeriod 125 (ns, if that unit is right); the controller
                                 currently runs at its slowest rate regardless */
    };
    spi_status_t s = spi_configure(&dev);
    if (s == SPI_OK) s = spi_transfer(&dev, NULL, frame, TOPCASE_FRAME_SIZE);
    out->status = (int)s;
    if (s != SPI_OK) return out->status;

    for (uint32_t i = 0; i < TOPCASE_FRAME_SIZE; i++) {
        if (frame[i] == 0x00u) out->zeros++;
        if (frame[i] == 0xFFu) out->ones++;
    }
    for (uint32_t i = 0; i < 16u; i++) out->head[i] = frame[i];
    out->crc_stored = (uint16_t)(frame[254] | (frame[255] << 8));
    out->crc_computed = crc16_arc(frame, 254u);
    return out->status;
}
