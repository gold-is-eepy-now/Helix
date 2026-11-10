/* virtio.c — the virtio 1.0 PCI transport (see virtio.h and docs/12-graphics.md).
 *
 * A virtio device has PCI vendor 0x1AF4. Vendor capabilities in its PCI config space say
 * where its registers are: each names a BAR and an offset inside it. The common
 * configuration holds the status byte, the feature bits and the queue addresses.
 *
 * A split virtqueue is three areas in one physically contiguous region: the descriptor
 * table, the "avail" ring where we post requests, and the "used" ring where the device
 * reports completions. We keep one request outstanding at a time, so a request is two
 * descriptors (the command, which the device reads; the response, which it writes), and
 * we wait for the used index to move before returning. */
#include "virtio.h"

#include <stddef.h>

#include "klog.h"
#include "kstring.h"
#include "pmm.h"
#include "time.h"
#include "vmm.h"

#define PCI_COMMAND     0x04u
#define PCI_CMD_MEMORY  0x2u
#define PCI_CMD_BUSMASTER 0x4u
#define PCI_CAP_PTR     0x34u
#define PCI_BAR0        0x10u
#define PCI_CAP_VENDOR  0x09u

#define CAP_COMMON      1u
#define CAP_NOTIFY      2u
#define CAP_DEVICE      4u

/* Offsets in the common configuration (virtio 1.0, section 4.1.4.3). */
#define C_DEVICE_FEATURE_SELECT 0x00u
#define C_DEVICE_FEATURE        0x04u
#define C_DRIVER_FEATURE_SELECT 0x08u
#define C_DRIVER_FEATURE        0x0Cu
#define C_DEVICE_STATUS         0x14u
#define C_QUEUE_SELECT          0x16u
#define C_QUEUE_SIZE            0x18u
#define C_QUEUE_ENABLE          0x1Cu
#define C_QUEUE_NOTIFY_OFF      0x1Eu
#define C_QUEUE_DESC            0x20u
#define C_QUEUE_DRIVER          0x28u
#define C_QUEUE_DEVICE          0x30u

#define ST_ACKNOWLEDGE  0x01u
#define ST_DRIVER       0x02u
#define ST_DRIVER_OK    0x04u
#define ST_FEATURES_OK  0x08u
#define ST_FAILED       0x80u

#define F_VERSION_1_HI  0x1u     /* bit 32 of the feature bits: bit 0 of the high word */

#define DESC_NEXT  1u
#define DESC_WRITE 2u

/* Each BAR we map gets its own window in the kernel's address space, 128 KiB apart. */
#define MMIO_WINDOW       0xFFFFA00002000000ULL
#define MMIO_WINDOW_SPAN  0x20000u
#define MMIO_PAGES        32u
#define PTE_PCD           (1ULL << 4)   /* page-level cache disable: device registers */

#define QUEUE_MAX 128u
#define AVAIL_OFF (16u * QUEUE_MAX)     /* the avail ring starts after the descriptors */
#define USED_OFF  4096u                 /* the used ring starts on the second page */

typedef struct {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} vring_desc_t;

static uint32_t cfg32(const pci_function_t *f, uint32_t off) {
    return pci_config_read32(f->bus, f->device, f->function, (uint8_t)off);
}

static uint8_t cfg8(const pci_function_t *f, uint32_t off) {
    return (uint8_t)(cfg32(f, off & ~3u) >> ((off & 3u) * 8u));
}

static uint8_t r8(volatile uint8_t *base, uint32_t off) { return *(volatile uint8_t *)(base + off); }
static uint16_t r16(volatile uint8_t *base, uint32_t off) { return *(volatile uint16_t *)(base + off); }
static uint32_t r32(volatile uint8_t *base, uint32_t off) { return *(volatile uint32_t *)(base + off); }
static void w8(volatile uint8_t *base, uint32_t off, uint8_t v) { *(volatile uint8_t *)(base + off) = v; }
static void w16(volatile uint8_t *base, uint32_t off, uint16_t v) { *(volatile uint16_t *)(base + off) = v; }
static void w32(volatile uint8_t *base, uint32_t off, uint32_t v) { *(volatile uint32_t *)(base + off) = v; }

/* Map BAR `bar` (32- or 64-bit memory BAR) into its window, once. */
static volatile uint8_t *map_bar(virtio_dev_t *dev, uint8_t bar) {
    if (bar >= 6) return NULL;
    if (dev->bar_virt[bar] != NULL) return dev->bar_virt[bar];
    uint32_t lo = cfg32(&dev->fn, PCI_BAR0 + 4u * bar);
    if ((lo & 1u) != 0) return NULL;                    /* I/O BAR: not used by virtio 1.0 */
    uint64_t phys = lo & ~0xFu;
    if (((lo >> 1) & 3u) == 2u) {                       /* 64-bit BAR: the high word follows */
        phys |= (uint64_t)cfg32(&dev->fn, PCI_BAR0 + 4u * (bar + 1u)) << 32;
    }
    uint64_t virt = MMIO_WINDOW + (uint64_t)bar * MMIO_WINDOW_SPAN;
    for (uint64_t p = 0; p < MMIO_PAGES; p++) {
        if (!vmm_map(virt + p * 4096u, phys + p * 4096u, VMM_PRESENT | VMM_WRITE | VMM_NX | PTE_PCD)) {
            klog_note(KLOG_DESKTOP, "[virtio] could not map BAR %u\n", bar);
            return NULL;
        }
    }
    dev->bar_virt[bar] = (volatile uint8_t *)(uintptr_t)virt;
    return dev->bar_virt[bar];
}

/* Walk the capability list and record where each virtio structure lives. */
static bool read_caps(virtio_dev_t *dev) {
    if ((cfg32(&dev->fn, 0x04) & (1u << 20)) == 0) return false;     /* no capability list */
    uint8_t pos = cfg8(&dev->fn, PCI_CAP_PTR) & 0xFCu;
    for (int guard = 0; pos != 0 && guard < 48; guard++) {
        if (cfg8(&dev->fn, pos) == PCI_CAP_VENDOR) {
            uint8_t type = cfg8(&dev->fn, pos + 3u);
            uint8_t bar = cfg8(&dev->fn, pos + 4u);
            uint32_t off = cfg32(&dev->fn, pos + 8u);
            uint32_t len = cfg32(&dev->fn, pos + 12u);
            volatile uint8_t *base = map_bar(dev, bar);
            if (base == NULL || off + len > MMIO_WINDOW_SPAN) return false;
            if (type == CAP_COMMON) dev->common = base + off;
            else if (type == CAP_NOTIFY) {
                dev->notify = base + off;
                dev->notify_mult = cfg32(&dev->fn, pos + 16u);
            } else if (type == CAP_DEVICE) dev->device = base + off;
        }
        pos = cfg8(&dev->fn, pos + 1u) & 0xFCu;
    }
    return dev->common != NULL && dev->notify != NULL;
}

bool virtio_open(virtio_dev_t *dev, uint16_t device_id) {
    memset(dev, 0, sizeof *dev);
    if (!pci_find_device((uint16_t)VIRTIO_VENDOR_ID, device_id, &dev->fn)) return false;

    uint32_t cmd = cfg32(&dev->fn, PCI_COMMAND);
    pci_config_write32(dev->fn.bus, dev->fn.device, dev->fn.function, PCI_COMMAND,
                       (cmd & 0xFFFFu) | PCI_CMD_MEMORY | PCI_CMD_BUSMASTER);
    if (!read_caps(dev)) {
        klog_note(KLOG_DESKTOP, "[virtio] device %04x has no usable register layout\n", device_id);
        return false;
    }

    /* The handshake from virtio 1.0 section 3.1.1: reset, acknowledge, feature negotiation. */
    w8(dev->common, C_DEVICE_STATUS, 0);
    for (int i = 0; r8(dev->common, C_DEVICE_STATUS) != 0 && i < 100000; i++) __asm__ volatile("pause");
    w8(dev->common, C_DEVICE_STATUS, ST_ACKNOWLEDGE);
    w8(dev->common, C_DEVICE_STATUS, ST_ACKNOWLEDGE | ST_DRIVER);

    w32(dev->common, C_DEVICE_FEATURE_SELECT, 1);
    if ((r32(dev->common, C_DEVICE_FEATURE) & F_VERSION_1_HI) == 0) {
        w8(dev->common, C_DEVICE_STATUS, ST_FAILED);
        return false;                                   /* a legacy-only device: not supported */
    }
    w32(dev->common, C_DRIVER_FEATURE_SELECT, 0);
    w32(dev->common, C_DRIVER_FEATURE, 0);              /* no optional features */
    w32(dev->common, C_DRIVER_FEATURE_SELECT, 1);
    w32(dev->common, C_DRIVER_FEATURE, F_VERSION_1_HI);

    w8(dev->common, C_DEVICE_STATUS, ST_ACKNOWLEDGE | ST_DRIVER | ST_FEATURES_OK);
    if ((r8(dev->common, C_DEVICE_STATUS) & ST_FEATURES_OK) == 0) {
        w8(dev->common, C_DEVICE_STATUS, ST_FAILED);
        return false;                                   /* the device refused our features */
    }
    return true;
}

bool virtio_setup_queue(virtio_dev_t *dev, uint16_t index, uint16_t max_size, virtq_t *q) {
    w16(dev->common, C_QUEUE_SELECT, index);
    uint16_t size = r16(dev->common, C_QUEUE_SIZE);
    if (size == 0) return false;
    if (size > max_size) size = max_size;
    if (size > QUEUE_MAX) size = QUEUE_MAX;
    w16(dev->common, C_QUEUE_SIZE, size);

    uintptr_t ring_phys = pmm_alloc_frames(2);          /* descriptors and avail (page 1), used (page 2) */
    uintptr_t buf_phys = pmm_alloc_frame();
    if (ring_phys == 0 || buf_phys == 0) return false;
    q->ring = (uint8_t *)vmm_phys_to_virt(ring_phys);
    q->buf = (uint8_t *)vmm_phys_to_virt(buf_phys);
    memset(q->ring, 0, 8192);
    memset(q->buf, 0, 4096);
    q->buf_phys = buf_phys;
    q->index = index;
    q->size = size;
    q->next_avail = 0;
    q->last_used = 0;

    uint16_t notify_off = r16(dev->common, C_QUEUE_NOTIFY_OFF);
    q->doorbell = (volatile uint16_t *)(dev->notify + (uint32_t)notify_off * dev->notify_mult);

    w32(dev->common, C_QUEUE_DESC, (uint32_t)ring_phys);
    w32(dev->common, C_QUEUE_DESC + 4, (uint32_t)(ring_phys >> 32));
    uint64_t avail = ring_phys + AVAIL_OFF;
    w32(dev->common, C_QUEUE_DRIVER, (uint32_t)avail);
    w32(dev->common, C_QUEUE_DRIVER + 4, (uint32_t)(avail >> 32));
    uint64_t used = ring_phys + USED_OFF;
    w32(dev->common, C_QUEUE_DEVICE, (uint32_t)used);
    w32(dev->common, C_QUEUE_DEVICE + 4, (uint32_t)(used >> 32));
    w16(dev->common, C_QUEUE_ENABLE, 1);
    return true;
}

void virtio_driver_ok(virtio_dev_t *dev) {
    w8(dev->common, C_DEVICE_STATUS,
       ST_ACKNOWLEDGE | ST_DRIVER | ST_FEATURES_OK | ST_DRIVER_OK);
}

bool virtio_submit(virtq_t *q, const void *req, uint32_t req_len, void *resp, uint32_t resp_len) {
    if (req_len > VIRTIO_BUF_MAX_REQ || resp_len > VIRTIO_BUF_MAX_RESP) return false;
    memcpy(q->buf, req, req_len);
    memset(q->buf + VIRTIO_RESP_OFF, 0, resp_len);

    vring_desc_t *desc = (vring_desc_t *)q->ring;
    desc[0].addr = q->buf_phys;
    desc[0].len = req_len;
    desc[0].flags = DESC_NEXT;
    desc[0].next = 1;
    desc[1].addr = q->buf_phys + VIRTIO_RESP_OFF;
    desc[1].len = resp_len;
    desc[1].flags = DESC_WRITE;
    desc[1].next = 0;

    volatile uint16_t *avail = (volatile uint16_t *)(q->ring + AVAIL_OFF);
    volatile uint16_t *used_idx = (volatile uint16_t *)(q->ring + USED_OFF + 2);
    avail[2 + (q->next_avail % q->size)] = 0;           /* head of the chain: descriptor 0 */
    __asm__ volatile("" ::: "memory");                  /* the entry must be visible before the index */
    q->next_avail++;
    avail[1] = q->next_avail;
    __asm__ volatile("" ::: "memory");
    *q->doorbell = q->index;

    uint64_t start = time_uptime_ms();
    while (*used_idx == q->last_used) {
        if (time_uptime_ms() - start > 2000) return false;
        __asm__ volatile("pause");
    }
    q->last_used++;
    memcpy(resp, q->buf + VIRTIO_RESP_OFF, resp_len);
    return true;
}
