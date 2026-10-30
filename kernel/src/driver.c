/* driver.c — driver framework implementation for Helix OS.
 *
 * M10 milestone: native driver framework and isolated drivers.
 *
 * This module provides a minimal driver registration system. Drivers can
 * register their name and initialization functions. Device objects created
 * by drivers are managed through the object manager and expose
 * Device.Control capabilities to user-space.
 *
 * Driver code runs in kernel context and must not block. All driver
 * initialization is performed before the shell is started.
 */

#include "driver.h"

#include "console.h"
#include "heap.h"
#include "object.h"

/* Reference object manager initialization flag */
extern bool g_initialized;

#define MAX_DRIVERS 16

static driver_t *g_drivers[MAX_DRIVERS];
static uint8_t g_driver_count;
static bool g_driver_initialized;

void driver_register(driver_t *driver) {
    if (!g_driver_initialized || g_driver_count >= MAX_DRIVERS) {
        return;
    }
    g_drivers[g_driver_count++] = driver;
}

void driver_init_all(void) {
    g_driver_initialized = true;
    for (uint8_t i = 0; i < g_driver_count; i++) {
        if (g_drivers[i]->init) {
            g_drivers[i]->init();
        }
    }
}

driver_t *driver_find(const char *name) {
    if (name == NULL) return NULL;
    for (uint8_t i = 0; i < g_driver_count; i++) {
        if (g_drivers[i] && g_drivers[i]->name &&
            g_drivers[i]->name[0] == name[0]) {
            /* Simple string compare */
            size_t j = 0;
            for (; g_drivers[i]->name[j] && name[j]; j++) {
                if (g_drivers[i]->name[j] != name[j]) {
                    break;
                }
            }
            if (g_drivers[i]->name[j] == '\0' && name[j] == '\0') {
                return g_drivers[i];
            }
        }
    }
    return NULL;
}

object_t *driver_get_device(driver_t *driver, const char *name) {
    (void)driver;
    (void)name;
    /* Base implementation: no device lookup.
     * Drivers with devices override this. */
    return NULL;
}

object_t *device_create(driver_t *driver, size_t payload_size,
                        object_destroy_fn destroy) {
    (void)driver;
    if (!g_driver_initialized) return NULL;
    return object_create(OBJECT_TYPE_DEVICE, payload_size, destroy);
}