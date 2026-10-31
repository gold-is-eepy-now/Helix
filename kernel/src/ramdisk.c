/* ramdisk.c — in-memory block device driver for Helix OS.
 *
 * M10/M11 milestone: driver framework and storage.
 *
 * A ramdisk driver that allocates backing memory from the kernel heap
 * and presents it as a block device. This provides a real block device
 * for testing storage and filesystems without hardware.
 *
 * Storage is limited to 2 MiB (4096 blocks of 512 bytes) to keep memory
 * usage low.
 */

#include "block_device.h"

#include "console.h"
#include "heap.h"
#include "kstring.h"

#define RAMDISK_BLOCK_SIZE 4096u  /* ext4 block size */
#define RAMDISK_BLOCK_COUNT 2048u  /* 8 MiB (2048 * 4096) */

typedef struct ramdisk {
    block_device_t base;
    uint8_t *data;
} ramdisk_t;

static ramdisk_t g_ramdisk;

static uint64_t ramdisk_read(block_device_t *dev, uint64_t lba,
                             void *buffer, uint64_t blocks) {
    ramdisk_t *rd = (ramdisk_t *)dev;
    if (lba >= RAMDISK_BLOCK_COUNT) return 0;
    if (lba + blocks > RAMDISK_BLOCK_COUNT) {
        blocks = RAMDISK_BLOCK_COUNT - lba;
    }
    for (uint64_t i = 0; i < blocks; i++) {
        memcpy(buffer + i * RAMDISK_BLOCK_SIZE,
               rd->data + lba * RAMDISK_BLOCK_SIZE,
               RAMDISK_BLOCK_SIZE);
    }
    return blocks;
}

static uint64_t ramdisk_write(block_device_t *dev, uint64_t lba,
                              const void *buffer, uint64_t blocks) {
    ramdisk_t *rd = (ramdisk_t *)dev;
    if (lba >= RAMDISK_BLOCK_COUNT) return 0;
    if (lba + blocks > RAMDISK_BLOCK_COUNT) {
        blocks = RAMDISK_BLOCK_COUNT - lba;
    }
    for (uint64_t i = 0; i < blocks; i++) {
        memcpy(rd->data + lba * RAMDISK_BLOCK_SIZE,
               buffer + i * RAMDISK_BLOCK_SIZE,
               RAMDISK_BLOCK_SIZE);
    }
    return blocks;
}

static bool ramdisk_flush(block_device_t *dev) {
    (void)dev;
    return true;
}

void ramdisk_init(void) {
    g_ramdisk.data = kzalloc(RAMDISK_BLOCK_COUNT * RAMDISK_BLOCK_SIZE);
    if (g_ramdisk.data == NULL) {
        console_write("[ramdisk] failed to allocate backing memory\n");
        return;
    }

    g_ramdisk.base.name = "ramdisk";
    g_ramdisk.base.block_size = RAMDISK_BLOCK_SIZE;
    g_ramdisk.base.block_count = RAMDISK_BLOCK_COUNT;
    g_ramdisk.base.read = ramdisk_read;
    g_ramdisk.base.write = ramdisk_write;
    g_ramdisk.base.flush = ramdisk_flush;

    console_write("[ramdisk] initialized: ");
    console_write_dec(RAMDISK_BLOCK_COUNT * RAMDISK_BLOCK_SIZE / 1024u);
    console_write(" KiB\n");
}

block_device_t *block_device_ramdisk(void) {
    return &g_ramdisk.base;
}