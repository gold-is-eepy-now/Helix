/* ramdisk_driver.c — ramdisk driver registration for Helix OS.
 *
 * M10 milestone: driver framework and isolated drivers.
 *
 * Registers the ramdisk block device with the system.
 */

#include "driver.h"
#include "block_device.h"
#include "console.h"

void ramdisk_init(void);
block_device_t *block_device_ramdisk(void);

static driver_t g_ramdisk_driver = {
    .name = "ramdisk",
    .init = ramdisk_init,
    .probe = NULL,
    .remove = NULL,
};

void ramdisk_driver_init(void) {
    driver_register(&g_ramdisk_driver);
}

void ramdisk_driver_removal(void) {
    /* Nothing to do for ramdisk */
}