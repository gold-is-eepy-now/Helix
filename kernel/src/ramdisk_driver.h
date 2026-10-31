/* ramdisk_driver.h — ramdisk driver registration interface. */
#ifndef HELIX_RAMDISK_DRIVER_H
#define HELIX_RAMDISK_DRIVER_H

#include "block_device.h"

void ramdisk_driver_init(void);
void ramdisk_driver_removal(void);
block_device_t *block_device_ramdisk(void);

#endif /* HELIX_RAMDISK_DRIVER_H */