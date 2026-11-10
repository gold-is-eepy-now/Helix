/* virtio.h — the virtio 1.0 PCI transport: find a device, set it up, run requests on
 * a split virtqueue. See docs/12-graphics.md. */
#ifndef HELIX_VIRTIO_H
#define HELIX_VIRTIO_H

#include <stdbool.h>
#include <stdint.h>

#include "pci.h"

#define VIRTIO_VENDOR_ID 0x1AF4u

typedef struct {
    pci_function_t fn;
    volatile uint8_t *common;        /* common configuration: status, features, queues */
    volatile uint8_t *notify;        /* base of the notification area */
    uint32_t notify_mult;            /* bytes per queue in the notification area */
    volatile uint8_t *device;        /* device-specific configuration (may be NULL) */
    volatile uint8_t *bar_virt[6];   /* each BAR we mapped, by BAR index */
} virtio_dev_t;

typedef struct {
    uint16_t index;                  /* queue number on the device */
    uint16_t size;                   /* entries (a power of two, at most 128 here) */
    uint16_t next_avail;             /* our avail index */
    uint16_t last_used;              /* how far we have read the used ring */
    volatile uint16_t *doorbell;     /* write the queue index here to kick the device */
    uint8_t *ring;                   /* descriptors, avail ring and used ring (virtual) */
    uint8_t *buf;                    /* request at 0, response at VIRTIO_RESP_OFF (virtual) */
    uint64_t buf_phys;               /* the same buffer, as the device sees it */
} virtq_t;

#define VIRTIO_RESP_OFF 1024u
#define VIRTIO_BUF_MAX_REQ  1024u
#define VIRTIO_BUF_MAX_RESP 3072u

/* Find the device with this PCI device ID, map its registers, and negotiate features
 * (only VIRTIO_F_VERSION_1 is accepted). Queues are set up after this. */
bool virtio_open(virtio_dev_t *dev, uint16_t device_id);

/* Allocate and program queue `index`, using at most max_size entries. */
bool virtio_setup_queue(virtio_dev_t *dev, uint16_t index, uint16_t max_size, virtq_t *q);

/* Tell the device the driver is ready. Call after every queue is set up. */
void virtio_driver_ok(virtio_dev_t *dev);

/* Send one request and wait (up to about two seconds) for the device's answer. The
 * request is copied into the queue's buffer, and the response is copied out. */
bool virtio_submit(virtq_t *q, const void *req, uint32_t req_len, void *resp, uint32_t resp_len);

#endif /* HELIX_VIRTIO_H */
