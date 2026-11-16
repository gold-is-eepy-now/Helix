/* nvme.h — NVM Express controller: one admin queue, one I/O queue pair, polled.
 *
 * Finds the first NVMe controller (PCI class 01/08/02), enables it, identifies the first
 * active namespace and registers it as a block device named nvmeN. Reads, writes and
 * flush work; there are no interrupts, no multiple queues, no namespace management and
 * no power-state handling. Written from the NVM Express Base Specification (controller
 * registers, queue and command formats, Identify, Create I/O Queue, Read, Write, Flush).
 * No other kernel's source was used. See docs/23-nvme.md. */
#ifndef HELIX_NVME_H
#define HELIX_NVME_H

#include <stdbool.h>
#include <stdint.h>

#include "block_device.h"

#define NVME_MAX_DISKS 2

typedef struct {
    block_device_t *dev;
    char model[41];
    char serial[21];
    char firmware[9];
    uint32_t block_size;
    uint64_t block_count;
    uint32_t max_blocks;         /* largest transfer in one command */
    uint32_t commands, errors;   /* commands submitted, commands that completed with an error */
} nvme_disk_t;

/* Returns true if at least one namespace was registered. Call once at boot after vmm_init. */
bool nvme_init(void);
int nvme_disk_count(void);
const nvme_disk_t *nvme_disk(int index);

#endif
