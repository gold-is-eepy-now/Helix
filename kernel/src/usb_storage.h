/* usb_storage.h — USB mass storage (Bulk-Only Transport, SCSI transparent command set).
 *
 * Read-only. Each disk found on the UHCI bus is registered as a block device named
 * usb0, usb1, ... Written from the USB Mass Storage Class Bulk-Only Transport 1.0
 * specification and the SCSI Block Commands (INQUIRY, TEST UNIT READY, REQUEST SENSE,
 * READ CAPACITY(10), READ(10)). No other kernel's source was used.
 */
#ifndef HELIX_USB_STORAGE_H
#define HELIX_USB_STORAGE_H

#include <stdint.h>

typedef struct block_device block_device_t;

typedef struct {
    block_device_t *dev;
    char vendor[9];
    char product[17];
    uint32_t block_size;
    uint64_t block_count;
    uint32_t commands, errors, resets;   /* BOT commands run, failed, reset recoveries */
} usb_disk_t;

/* Probe every USB device with a mass-storage interface. Call after usb_host_init(). */
void usb_storage_init(void);
int usb_storage_disk_count(void);
const usb_disk_t *usb_storage_disk(int i);

#endif
