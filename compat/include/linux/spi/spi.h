/* linux/spi/spi.h — Linux's SPI driver model over Helix's SPI layer.
 *
 * What exists: transfer/message structs, a driver register that matches a device by ACPI id,
 * and spi_async() that queues a message for compat_spi_pump(), which "executes" it through
 * the real Helix controller path only when one is attached (none is wired yet: the pump
 * completes messages with -ENODEV). See compat/src/spi_shim.c. */
#ifndef HELIX_COMPAT_LINUX_SPI_SPI_H
#define HELIX_COMPAT_LINUX_SPI_SPI_H
#include <linux/acpi.h>
#include <linux/module.h>

#define SPI_DELAY_UNIT_USECS 0
struct spi_delay { u16 value; u8 unit; };

struct spi_transfer {
    const void *tx_buf;
    void *rx_buf;
    unsigned int len;
    unsigned int cs_change : 1;
    struct spi_delay delay;
    struct spi_transfer *next;
};

struct spi_message {
    struct spi_transfer *first;
    struct spi_transfer *last;
    void (*complete)(void *context);
    void *context;
    int status;
    unsigned int actual_length;
};

struct spi_device {
    struct device dev;
    u32 max_speed_hz;
    u8 chip_select;
    const char *modalias;
};

struct spi_driver {
    struct device_driver driver;
    int (*probe)(struct spi_device *spi);
    void (*remove)(struct spi_device *spi);
    void (*shutdown)(struct spi_device *spi);
};

#define to_spi_device(d) container_of((d), struct spi_device, dev)
static inline void spi_set_drvdata(struct spi_device *spi, void *data) { dev_set_drvdata(&spi->dev, data); }
static inline void *spi_get_drvdata(const struct spi_device *spi) { return dev_get_drvdata(&spi->dev); }

static inline void spi_message_init(struct spi_message *m) {
    m->first = m->last = NULL; m->complete = NULL; m->context = NULL; m->status = 0; m->actual_length = 0;
}
static inline void spi_message_add_tail(struct spi_transfer *t, struct spi_message *m) {
    t->next = NULL;
    if (m->last) m->last->next = t; else m->first = t;
    m->last = t;
}

int spi_async(struct spi_device *spi, struct spi_message *message);
int compat_spi_register_driver(struct spi_driver *drv);
/* Make a device and offer it to registered drivers whose ACPI table lists acpi_hid.
 * Returns the probe result, or -ENODEV when no driver matches. */
int compat_spi_add_device(struct spi_device *spi, const char *acpi_hid);
/* Helix addition: bind the AML device with this _HID to a slave and probe it. -ENOENT if none. */
int compat_spi_scan_acpi(const char *acpi_hid);
/* Complete queued messages. Returns how many ran. */
int compat_spi_pump(void);

#define module_spi_driver(drv) \
    static int __init drv##_init_fn(void) { return compat_spi_register_driver(&(drv)); } \
    module_init(drv##_init_fn);
#endif
