/* block_device.c — block device abstraction for Helix OS.
 *
 * M10/M11 milestone: driver framework and storage.
 *
 * Provides a minimal block device registry. Block devices register
 * themselves with name, block size, and block count. Operations read/write
 * blocks directly through the driver-supplied callbacks.
 */

#include "block_device.h"

#include "console.h"

#include "heap.h"

#define MAX_BLOCK_DEVICES 8

static block_device_t *g_block_devices[MAX_BLOCK_DEVICES];
static uint8_t g_block_device_count;

void block_device_init_all(void) {
    /* No-op for now: block devices are initialized by their drivers.
     * This hook is here for future use. */
}

block_device_t *block_device_register(const char *name, uint64_t block_size,
                                      uint64_t block_count) {
    if (g_block_device_count >= MAX_BLOCK_DEVICES) return NULL;
    block_device_t *dev = kzalloc(sizeof(*dev));
    if (dev == NULL) return NULL;
    dev->name = name;
    dev->block_size = block_size;
    dev->block_count = block_count;
    g_block_devices[g_block_device_count++] = dev;
    return dev;
}

void block_device_unregister(block_device_t *dev) {
    for (uint8_t i = 0; i < g_block_device_count; i++) {
        if (g_block_devices[i] == dev) {
            for (uint8_t j = i + 1; j < g_block_device_count; j++) {
                g_block_devices[j - 1] = g_block_devices[j];
            }
            g_block_devices[--g_block_device_count] = NULL;
            kfree(dev);
            return;
        }
    }
}

object_t *block_device_get_object(block_device_t *dev) {
    return dev ? dev->device_obj : NULL;
}

uint64_t block_device_read(block_device_t *dev, uint64_t lba,
                           void *buffer, uint64_t blocks) {
    if (dev == NULL || dev->read == NULL || buffer == NULL) return 0;
    return dev->read(dev, lba, buffer, blocks);
}

uint64_t block_device_write(block_device_t *dev, uint64_t lba,
                            const void *buffer, uint64_t blocks) {
    if (dev == NULL || dev->write == NULL || buffer == NULL) return 0;
    return dev->write(dev, lba, buffer, blocks);
}