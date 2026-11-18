/* ahci.h — AHCI (SATA) host controller, stage 1: discover and read.
 *
 * The first AHCI controller on the PCI bus (class 01/06/01) is initialised. Each port
 * with a SATA disk is identified with ATA IDENTIFY DEVICE and registered as a block
 * device named sataN. Reads go through DMA with polled completion. Writes are not
 * implemented yet and return 0. See docs/20-ahci.md. */
#ifndef HELIX_AHCI_H
#define HELIX_AHCI_H

#include <stdbool.h>
#include <stdint.h>

#include "block_device.h"

#define AHCI_MAX_DISKS 8

typedef struct {
    block_device_t *dev;   /* the registered block device, named sataN */
    char model[41];        /* the drive's model string, trimmed */
    uint64_t sectors;      /* capacity in 512-byte sectors */
} ahci_disk_t;

/* Finds the controller and identifies its disks. Returns false when there is no AHCI
 * controller. Safe to call once, at boot. */
bool ahci_init(void);

int ahci_disk_count(void);
const ahci_disk_t *ahci_disk(int index);

#endif /* HELIX_AHCI_H */
