/* block_device.h — block device abstraction for Helix OS.
 *
 * M10/M11 milestone: driver framework and storage.
 *
 * - block_device_t: core block device structure
 * - block_device_register: register a block device
 * - block_device_read: read blocks from a device
 * - block_device_write: write blocks to a device
 * - block_device_get_info: get device geometry
 * - Block devices create OBJECT_TYPE_BLOCK_DEVICE objects
 */

#ifndef HELIX_BLOCK_DEVICE_H
#define HELIX_BLOCK_DEVICE_H

#include "object.h"

typedef struct block_device block_device_t;

struct block_device {
    const char *name;
    uint64_t block_size;
    uint64_t block_count;
    object_t *device_obj;

    /* Read blocks. Returns number of blocks read, or 0 on error. */
    uint64_t (*read)(block_device_t *dev, uint64_t lba, void *buffer,
                     uint64_t blocks);

    /* Write blocks. Returns number of blocks written, or 0 on error. */
    uint64_t (*write)(block_device_t *dev, uint64_t lba, const void *buffer,
                      uint64_t blocks);

    /* Optional: flush cached data. Returns true on success. */
    bool (*flush)(block_device_t *dev);

    /* Optional: device-specific control. */
    int (*ioctl)(block_device_t *dev, uint32_t cmd, void *arg);
};

void block_device_init_all(void);
block_device_t *block_device_register(const char *name, uint64_t block_size,
                                      uint64_t block_count);
void block_device_unregister(block_device_t *dev);

/* Get a block device object handle */
object_t *block_device_get_object(block_device_t *dev);

/* Read/write helpers using the object handle */
uint64_t block_device_read(block_device_t *dev, uint64_t lba,
                           void *buffer, uint64_t blocks);
uint64_t block_device_write(block_device_t *dev, uint64_t lba,
                            const void *buffer, uint64_t blocks);

#endif /* HELIX_BLOCK_DEVICE_H */