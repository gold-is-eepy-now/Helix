/* spi.c — the generic SPI layer. Argument checks live here so that every controller
 * gets them; the controller only sees arguments it is allowed to act on. */
#include "spi.h"

static spi_controller_t *g_ctrl[SPI_MAX_CONTROLLERS];
static unsigned g_count;

bool spi_register_controller(spi_controller_t *c) {
    if (c == NULL || c->transfer == NULL || c->set_mode == NULL) return false;
    if (g_count >= SPI_MAX_CONTROLLERS) return false;
    g_ctrl[g_count++] = c;
    return true;
}

unsigned spi_controller_count(void) {
    return g_count;
}

spi_controller_t *spi_controller_at(unsigned i) {
    return i < g_count ? g_ctrl[i] : NULL;
}

spi_status_t spi_configure(spi_device_t *dev) {
    if (dev == NULL || dev->ctrl == NULL) return SPI_ERR_BAD_ARG;
    if (dev->bits != 8u) return SPI_ERR_BAD_ARG;
    if (dev->mode > 3u) return SPI_ERR_BAD_ARG;
    if (!dev->ctrl->ready) return SPI_ERR_NOT_READY;
    return dev->ctrl->set_mode(dev->ctrl, dev->chip_select, dev->mode, dev->speed_hz, dev->bits);
}

spi_status_t spi_transfer(spi_device_t *dev, const uint8_t *tx, uint8_t *rx, uint32_t len) {
    if (dev == NULL || dev->ctrl == NULL) return SPI_ERR_BAD_ARG;
    if (!dev->ctrl->ready) return SPI_ERR_NOT_READY;
    if (len == 0) return SPI_OK;
    return dev->ctrl->transfer(dev->ctrl, dev->chip_select, tx, rx, len);
}

const char *spi_status_name(spi_status_t s) {
    switch (s) {
    case SPI_OK:           return "ok";
    case SPI_ERR_NOT_READY:return "controller not ready";
    case SPI_ERR_BAD_ARG:  return "bad argument";
    case SPI_ERR_TIMEOUT:  return "timed out";
    case SPI_ERR_OVERRUN:  return "receive overrun";
    }
    return "unknown";
}
