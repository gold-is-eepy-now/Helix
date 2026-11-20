/* spi_shim.c — Linux-style SPI drivers over Helix. See compat/include/linux/spi/spi.h.
 *
 * A message queued with spi_async() is run by compat_spi_pump() through Helix's kernel SPI
 * layer (kernel/src/spi.c). The route from the device to the hardware is:
 *   1. compat_spi_scan_acpi() finds the slave in the AML namespace by its _HID and links it
 *      (compat/src/spi_acpi.c): the slave's _CRS gives its chip select and clock settings, and
 *      its parent's _ADR must name a registered controller. Without that link nothing is sent.
 *   2. A message is accepted only from the device that was linked. Each queued message keeps its
 *      device, so another device's message fails with -ENODEV instead of using the wrong wiring.
 *   3. A transfer that holds chip select (cs_change == 0) before a later transfer is refused with
 *      -EOPNOTSUPP. The LPSS driver frames each transfer separately, so it cannot hold the line.
 *
 * Errors: a missing or unlinked controller gives -ENODEV, a bad argument -EINVAL, a controller
 * timeout or overrun -EIO. A message is never reported as sent when it was not. */
#include <linux/spi/spi.h>
#include <linux/workqueue.h>
#include <linux/errno.h>
#include <linux/acpi.h>
#include <linux/compat_common.h>
#include <stdbool.h>
#include "kstring.h"
#include "panic.h"       /* kprintf */
#include "spi.h"         /* Helix's kernel SPI layer */
#include "spi_acpi.h"

#define QUEUE_SIZE 8
#define MAX_DRIVERS 4

static struct spi_message *g_queue[QUEUE_SIZE];
static struct spi_device *g_qdev[QUEUE_SIZE];   /* the device each queued message came from */
static unsigned g_head, g_tail;
static struct spi_driver *g_drivers[MAX_DRIVERS];
static int g_driver_count;
static int g_probe_calls;

/* The one slave this bridge serves: its device, and the wiring worked out from ACPI. */
static bool g_bound;
static struct spi_device *g_bound_dev;
static spi_link_t g_link;

/* While a test borrows the bridge, the real slave's binding is kept here and put back after. */
static bool g_saved_bound;
static struct spi_device *g_saved_dev;
static spi_link_t g_saved_link;

static spi_device_t g_kdev;

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
static void run_message(struct spi_message *m, struct spi_device *dev) {
    m->status = 0;
    m->actual_length = 0;

    if (!g_bound || dev != g_bound_dev) { m->status = -ENODEV; return; }
    if (g_link.status != SPI_LINK_OK || g_link.ctrl == NULL) { m->status = -ENODEV; return; }
    if (g_link.res.chip_select > 0xFFu) { m->status = -EINVAL; return; }   /* kernel field is 8 bits */

    g_kdev.ctrl = g_link.ctrl;
    g_kdev.chip_select = (uint8_t)g_link.res.chip_select;
    g_kdev.mode = (uint8_t)(((g_link.res.clock_polarity & 1u) << 1) | (g_link.res.clock_phase & 1u));
    g_kdev.bits = 8;
    g_kdev.speed_hz = g_link.res.speed_hz;   /* the LPSS driver ignores it; see lpss_set_mode */

    m->status = status_to_errno(spi_configure(&g_kdev));
    if (m->status != 0) return;

    for (struct spi_transfer *t = m->first; t != NULL; t = t->next) {
        if (!t->cs_change && t->next != NULL) {
            m->status = -EOPNOTSUPP;
            return;
        }
        spi_status_t s = spi_transfer(&g_kdev, (const uint8_t *)t->tx_buf, (uint8_t *)t->rx_buf,
                                      (uint32_t)t->len);
        if (s != SPI_OK) {
            m->status = status_to_errno(s);
            return;
        }
        m->actual_length += t->len;
    }
}

int spi_async(struct spi_device *spi, struct spi_message *message) {
    unsigned next = (g_head + 1u) % QUEUE_SIZE;
    if (next == g_tail) return -ENOSPC;
    g_queue[g_head] = message;
    g_qdev[g_head] = spi;
    g_head = next;
    return 0;
}

int compat_spi_pump(void) {
    int ran = 0;
    while (g_tail != g_head) {
        struct spi_message *m = g_queue[g_tail];
        struct spi_device *dev = g_qdev[g_tail];
        g_tail = (g_tail + 1u) % QUEUE_SIZE;
        run_message(m, dev);
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

static void bind_slave(struct spi_device *dev, aml_node_t *node) {
    spi_link_node(node, &g_link);
    g_bound = true;
    g_bound_dev = dev;
}

/* Returns -ENOENT when no device with the id is in the namespace, otherwise the probe's result.
 * The link is worked out first, so the driver's first message already has its wiring; if the link
 * fails, the message fails with -ENODEV and compat_spi_print_link says why. */
int compat_spi_scan_acpi(const char *acpi_hid) {
    static struct spi_device slave;
    acpi_handle node = compat_acpi_find_hid(acpi_hid);
    if (node == NULL) return -ENOENT;
    memset(&slave, 0, sizeof slave);
    slave.dev.acpi_node = node;
    bind_slave(&slave, (aml_node_t *)node);
    return compat_spi_add_device(&slave, acpi_hid);
}

/* Prints the link for a device, without probing it or sending anything. Used by the spi command. */
void compat_spi_print_link(const char *acpi_hid) {
    acpi_handle node = compat_acpi_find_hid(acpi_hid);
    if (node == NULL) {
        kprintf("spi: %s is not in the ACPI namespace\n", acpi_hid);
        return;
    }
    spi_link_t l;
    spi_link_node((aml_node_t *)node, &l);
    char pname[5];
    aml_node_name(l.parent, pname);
    kprintf("spi: %s is under %s\n", acpi_hid, pname);
    if (l.have_parent_adr) kprintf("spi: %s _ADR 0x%08x\n", pname, (unsigned)l.parent_adr);
    if (l.have_res) {
        kprintf("spi: _CRS chip select %u, clock phase %u, polarity %u, %u bits, %u Hz\n",
                (unsigned)l.res.chip_select, (unsigned)l.res.clock_phase,
                (unsigned)l.res.clock_polarity, (unsigned)l.res.data_bit_length,
                (unsigned)l.res.speed_hz);
    }
    if (l.status != SPI_LINK_OK) {
        kprintf("spi: not linked: %s\n", spi_link_status_text(l.status));
        return;
    }
    kprintf("spi: linked to controller %s\n", l.ctrl->name);
}

/* Test only: lend the bridge to a fake controller, with no ACPI slave. The messages in the test
 * use no device, which is the device this binding names. compat_spi_test_end puts back the real
 * slave's binding, so the applespi binding survives the test. */
void compat_spi_test_begin(spi_controller_t *c) {
    g_saved_bound = g_bound;
    g_saved_dev = g_bound_dev;
    g_saved_link = g_link;
    memset(&g_link, 0, sizeof g_link);
    g_link.status = SPI_LINK_OK;
    g_link.ctrl = c;
    g_link.have_res = true;
    g_link.res.chip_select = 0;
    g_link.res.clock_phase = 0;
    g_link.res.clock_polarity = 0;
    g_link.res.data_bit_length = 8;
    g_bound = true;
    g_bound_dev = NULL;
}

void compat_spi_test_end(void) {
    g_bound = g_saved_bound;
    g_bound_dev = g_saved_dev;
    g_link = g_saved_link;
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
