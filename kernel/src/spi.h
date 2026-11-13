/* spi.h — generic SPI: controllers, devices and transfers. No hardware here.
 *
 * A controller (for example the Intel LPSS GSPI block, intel_lpss_gspi.c) registers
 * itself with the bus. A device names its controller and chip-select line; transfers
 * go through spi_transfer(), which checks arguments and calls the controller.
 *
 * Only 8-bit words and polled, programmed I/O are supported so far. The interface
 * does not promise DMA, interrupts or any particular FIFO depth. */
#ifndef HELIX_SPI_H
#define HELIX_SPI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SPI_MAX_CONTROLLERS 4u

typedef enum {
    SPI_OK = 0,
    SPI_ERR_NOT_READY,     /* the controller did not initialise, or was not found */
    SPI_ERR_BAD_ARG,       /* unsupported word size or mode, or no controller */
    SPI_ERR_TIMEOUT,       /* a bounded wait for the hardware expired */
    SPI_ERR_OVERRUN        /* receive FIFO overran: data was lost */
} spi_status_t;

typedef struct spi_controller spi_controller_t;

struct spi_controller {
    const char *name;
    bool ready;            /* set by the driver once initialisation has verified the registers */
    /* Program clock polarity/phase, word size and clock rate for chip-select cs. */
    spi_status_t (*set_mode)(spi_controller_t *c, uint8_t cs, uint8_t mode, uint32_t speed_hz,
                             uint8_t bits);
    /* Full-duplex transfer of len bytes. tx may be NULL (sends zeros); rx may be NULL
     * (discards what is read). */
    spi_status_t (*transfer)(spi_controller_t *c, uint8_t cs, const uint8_t *tx, uint8_t *rx,
                             uint32_t len);
    void *priv;            /* the driver's own state */
};

typedef struct {
    spi_controller_t *ctrl;
    uint8_t chip_select;
    uint8_t mode;          /* bit 0 CPHA (phase), bit 1 CPOL (polarity) */
    uint8_t bits;          /* word size; 8 is the only one supported so far */
    uint32_t speed_hz;     /* requested clock; see the controller for what it can do */
} spi_device_t;

/* Add a controller to the bus. False when the table is full or c has no transfer hook. */
bool spi_register_controller(spi_controller_t *c);
unsigned spi_controller_count(void);
spi_controller_t *spi_controller_at(unsigned i);

/* Program the device's mode on its controller, then transfer. */
spi_status_t spi_configure(spi_device_t *dev);
spi_status_t spi_transfer(spi_device_t *dev, const uint8_t *tx, uint8_t *rx, uint32_t len);

const char *spi_status_name(spi_status_t s);

#endif /* HELIX_SPI_H */
