/* spi_shim.c — Linux-style SPI drivers over Helix. See compat/include/linux/spi/spi.h.
 *
 * Helix has an SPI controller layer (kernel/src/spi.c, Intel LPSS GSPI), but no transfer path
 * reaches an ACPI-described slave yet. So a message queued with spi_async() is completed by
 * compat_spi_pump() with -ENODEV: the transfer did not happen. A driver that needs a real
 * transfer will see that error and must refuse, which applespi does.
 *
 * compat_spi_scan_acpi() finds an ACPI slave by its _HID in the AML namespace and binds the
 * node to the device, so the driver's ACPI method calls reach the right object. */
#include <linux/spi/spi.h>
#include <linux/workqueue.h>
#include <linux/errno.h>
#include <linux/acpi.h>
#include <linux/compat_common.h>
#include "kstring.h"

#define QUEUE_SIZE 8
#define MAX_DRIVERS 4

static struct spi_message *g_queue[QUEUE_SIZE];
static unsigned g_head, g_tail;
static struct spi_driver *g_drivers[MAX_DRIVERS];
static int g_driver_count;
static int g_probe_calls;

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
        m->status = -ENODEV;
        m->actual_length = 0;
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
