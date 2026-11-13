/* apple_topcase_spi.h — the Apple top-case keyboard and trackpad on the Intel GSPI bus.
 *
 * Stage 4 of the Air work. This file holds only the read-only probe so far: it clocks one
 * 256-byte frame out of the device with the output held at zero, and reports what came back.
 * It never writes a command to the device. See docs/18-topcase.md. */
#ifndef HELIX_APPLE_TOPCASE_SPI_H
#define HELIX_APPLE_TOPCASE_SPI_H

#include <stdint.h>

#define TOPCASE_FRAME_SIZE 256u

typedef struct {
    int status;                 /* spi_status_t from spi.h; 0 is SPI_OK */
    uint32_t zeros;             /* bytes that were 0x00 */
    uint32_t ones;              /* bytes that were 0xFF */
    uint8_t head[16];           /* the first 16 bytes */
    uint16_t crc_stored;        /* last two bytes of the frame, little-endian */
    uint16_t crc_computed;      /* CRC-16 over the first 254 bytes */
} topcase_probe_t;

/* Read one frame. Returns the status; fills out for the shell to print. */
int topcase_probe(topcase_probe_t *out);

#endif /* HELIX_APPLE_TOPCASE_SPI_H */
