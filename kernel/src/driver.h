/* driver.h — driver framework for Helix OS.
 *
 * M10 milestone: native driver framework and isolated drivers.
 *
 * - driver_t: core driver structure with name, init, probe methods
 * - driver_register: register a driver with the system
 * - driver_init_all: initialize all registered drivers
 * - driver_find: find a driver by name
 * - driver_get_device: get a device object from a driver
 *
 * Drivers register with the system and create device objects that are
 * managed through the object manager. Devices expose 'Device.Control'
 * capabilities.
 */
#ifndef HELIX_DRIVER_H
#define HELIX_DRIVER_H

#include "object.h"

/* Forward declared in object.h: object_t */
typedef struct driver driver_t;

struct driver {
    const char *name;
    void (*init)(void);
    void (*probe)(driver_t *driver, object_t *device);
    void (*remove)(driver_t *driver, object_t *device);
};

void driver_register(driver_t *driver);
void driver_init_all(void);
driver_t *driver_find(const char *name);
object_t *driver_get_device(driver_t *driver, const char *name);

/* Create a device object owned by this driver. */
object_t *device_create(driver_t *driver, size_t payload_size,
                        object_destroy_fn destroy);

#endif /* HELIX_DRIVER_H */