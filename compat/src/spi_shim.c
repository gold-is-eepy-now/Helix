/* spi_shim.c — Linux-style SPI drivers over Helix. See compat/include/linux/spi/spi.h.
 *
 * A message queued with spi_async() is run by compat_spi_pump() through Helix's kernel SPI
 * layer (kernel/src/spi.c). The bridge is deliberately narrow:
 *   - Exactly one SPI controller must be registered (today: the Intel LPSS GSPI block, when it
 *     exists). The ACPI parent link from APP000D to its controller is not parsed yet, so with
 *     zero or several controllers the message completes with -ENODEV rather than guessing.
 *   - Chip select 0, mode 0, 8 bits, the controller's fixed clock. Values from the device's
 *     _CRS and _DSD are not read yet.
 *   - The LPSS driver frames each transfer separately, so chip select is released between
 *     transfers. A transfer that asks to hold chip select (cs_change == 0) before a later one
 *     is refused with -EOPNOTSUPP instead of being run with the wrong framing.
 *
 * compat_spi_scan_acpi() finds an ACPI slave by its _HID in the AML namespace and binds the
 * node to the device, so the driver's ACPI method calls reach the right object. */
#include <linux/spi/spi.h>
#include <linux/workqueue.h>
#include <linux/errno.h>
#include <linux/acpi.h>
#include <linux/compat_common.h>
#include "kstring.h"
#include "spi.h"   /* Helix's kernel SPI layer */

#define QUEUE_SIZE 8
#define MAX_DRIVERS 4

static struct spi_message *g_queue[QUEUE_SIZE];
static unsigned g_head, g_tail;
static struct spi_driver *g_drivers[MAX_DRIVERS];
static int g_driver_count;
static int g_probe_calls;

/* The kernel device used for every slave message. Filled in per message, so the controller
 * is looked up when it is needed, not cached from an earlier boot step. */
static spi_device_t g_kdev;

/* The one controller a slave can use, or NULL when there is not exactly one. */
static spi_device_t *kernel_device_for_slave(void) {
    if (spi_controller_count() != 1u) return NULL;
    g_kdev.ctrl = spi_controller_at(0);
    g_kdev.chip_select = 0;
    g_kdev.mode = 0;
    g_kdev.bits = 8;
    g_kdev.speed_hz = 0;   /* the LPSS driver ignores the rate; see lpss_set_mode */
    return &g_kdev;
}

static int status_to_errno(spi_status_t s) {
    switch (s) {
    case SPI_OK:            return 0;
    case SPI_ERR_NOT_READY: return -ENODEV;   /* controller absent or did not initialise */
    case SPI_ERR_BAD_ARG:   return -EINVAL;
    case SPI_ERR_TIMEOUT:   return -EIO;
    case SPI_ERR_OVERRUN:   return -EIO;
    }
    return -EIO;
}

/* Runs one message to completion, setting status and actual_length. */
static void run_message(struct spi_message *m) {
    m->status = 0;
    m->actual_length = 0;

    spi_device_t *dev = kernel_device_for_slave();
    if (dev == NULL) {
        m->status = -ENODEV;   /* no controller, or more than one: nothing can move bytes */
        return;
    }
    m->status = status_to_errno(spi_configure(dev));
    if (m->status != 0) return;

    for (struct spi_transfer *t = m->first; t != NULL; t = t->next) {
        if (!t->cs_change && t->next != NULL) {
            m->status = -EOPNOTSUPP;
            return;
        }
        spi_status_t s = spi_transfer(dev, (const uint8_t *)t->tx_buf, (uint8_t *)t->rx_buf,
                                      (uint32_t)t->len);
        if (s != SPI_OK) {
            m->status = status_to_errno(s);
            return;
        }
        m->actual_length += t->len;
    }
}

int spi_async(struct spi_device *spi, struct spi_message *message) {
    (void)spi;
    unsigned next = (g_head + 1u) % QUEUE_SIZE;
    if (next == g_tail) return -ENOSPC;
    g_queue[g_head] = message;
    g_head = next;
    return 0;
}

int compat_spi_pump(void) {
    int ran = 0;
    while (g_tail != g_head) {
        struct spi_message *m = g_queue[g_tail];
        g_tail = (g_tail + 1u) % QUEUE_SIZE;
        run_message(m);
        /* Completion runs with no lock held: it may queue another message. */
        if (m->complete) m->complete(m->context);
        ran++;
    }
    compat_work_run();
    return ran;
}

int compat_spi_register_driver(struct spi_driver *drv) {
    if (drv == NULL || g_driver_count >= MAX_DRIVERS) return -ENOMEM;
    g_drivers[g_driver_count++] = drv;
    return 0;
}

int compat_spi_driver_count(void) { return g_driver_count; }
int compat_spi_probe_calls(void) { return g_probe_calls; }

/* Returns -ENOENT when no device with the id is in the namespace, otherwise the probe's result. */
int compat_spi_scan_acpi(const char *acpi_hid) {
    static struct spi_device slave;
    acpi_handle node = compat_acpi_find_hid(acpi_hid);
    if (node == NULL) return -ENOENT;
    memset(&slave, 0, sizeof slave);
    slave.dev.acpi_node = node;
    return compat_spi_add_device(&slave, acpi_hid);
}

int compat_spi_add_device(struct spi_device *spi, const char *acpi_hid) {
    if (spi == NULL || acpi_hid == NULL) return -EINVAL;
    spi->dev.name = acpi_hid;
    for (int d = 0; d < g_driver_count; d++) {
        const struct acpi_device_id *ids = g_drivers[d]->driver.acpi_match_table;
        if (ids == NULL) continue;
        for (; ids->id[0] != '\0'; ids++) {
            if (strcmp(ids->id, acpi_hid) == 0) {
                g_probe_calls++;
                return g_drivers[d]->probe(spi);
            }
        }
    }
    return -ENODEV;
}
