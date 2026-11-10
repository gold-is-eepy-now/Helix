/* virtio_gpu.c — the virtio-gpu device (see virtio_gpu.h and docs/12-graphics.md).
 *
 * Every command is a header plus a body, sent on the control queue. Commands that only
 * report success answer with a bare header (RESP_OK_NODATA). The resource is ours: a
 * contiguous block of pages that the kernel draws into. The device reads it through DMA
 * when we send a TRANSFER_TO_HOST_2D, and shows the flushed rectangle when we send a
 * RESOURCE_FLUSH. */
#include "virtio_gpu.h"

#include <stddef.h>

#include "klog.h"
#include "kstring.h"
#include "pmm.h"
#include "virtio.h"
#include "vmm.h"

#define VIRTIO_GPU_DEVICE_ID 0x1050u         /* virtio-gpu, modern (non-transitional) */
#define VIRTIO_GPU_MAX_SCANOUTS 16u

#define CMD_GET_DISPLAY_INFO       0x0100u
#define CMD_RESOURCE_CREATE_2D     0x0101u
#define CMD_SET_SCANOUT            0x0103u
#define CMD_RESOURCE_FLUSH         0x0104u
#define CMD_TRANSFER_TO_HOST_2D    0x0105u
#define CMD_RESOURCE_ATTACH_BACKING 0x0106u
#define RESP_OK_NODATA             0x1100u
#define RESP_OK_DISPLAY_INFO       0x1101u

#define FORMAT_B8G8R8X8_UNORM      2u        /* bytes B G R X: the same packing fb.c uses */
#define DISPLAY_RESOURCE           1u        /* our one resource id */

typedef struct {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint8_t ring_idx;
    uint8_t padding[3];
} gpu_ctrl_hdr_t;                         /* 24 bytes */

typedef struct {
    uint32_t x, y, width, height;
} gpu_rect_t;                             /* 16 bytes */

typedef struct {
    uint32_t x, y, width, height;
    uint32_t enabled;
    uint32_t flags;
} gpu_display_one_t;                      /* 24 bytes */

typedef struct {
    gpu_ctrl_hdr_t hdr;
    gpu_display_one_t pmodes[VIRTIO_GPU_MAX_SCANOUTS];
} gpu_display_info_t;                     /* 408 bytes */

typedef struct {
    gpu_ctrl_hdr_t hdr;
    uint32_t resource_id, format, width, height;
} gpu_create_2d_t;                        /* 40 bytes */

typedef struct {
    gpu_ctrl_hdr_t hdr;
    uint32_t resource_id, nr_entries;
    uint64_t addr;                        /* one memory entry: address and length */
    uint32_t length, padding;
} gpu_attach_t;                           /* 48 bytes */

typedef struct {
    gpu_ctrl_hdr_t hdr;
    gpu_rect_t r;
    uint32_t scanout_id, resource_id;
} gpu_set_scanout_t;                      /* 48 bytes */

typedef struct {
    gpu_ctrl_hdr_t hdr;
    gpu_rect_t r;
    uint64_t offset;                      /* where the rectangle starts in the backing memory */
    uint32_t resource_id, padding;
} gpu_transfer_t;                         /* 56 bytes */

typedef struct {
    gpu_ctrl_hdr_t hdr;
    gpu_rect_t r;
    uint32_t resource_id, padding;
} gpu_flush_t;                            /* 48 bytes */

static struct {
    bool ready;
    uint32_t count;
    uint32_t width[VIRTIO_GPU_MAX_SCANOUTS];
    uint32_t height[VIRTIO_GPU_MAX_SCANOUTS];
    bool enabled[VIRTIO_GPU_MAX_SCANOUTS];
} g_gpu;

static virtq_t g_ctrl;                    /* the control queue, set up by virtio_gpu_init */
static bool g_queue_ready;

static struct {
    bool ready;
    uint8_t *pixels;
    uint64_t phys;
    uint32_t width, height, pitch;
} g_disp;

/* Send a command and check the device answered with a bare OK. */
static bool command_ok(const void *req, uint32_t len) {
    if (!g_queue_ready) return false;
    gpu_ctrl_hdr_t resp;
    if (!virtio_submit(&g_ctrl, req, len, &resp, sizeof resp)) return false;
    if (resp.type != RESP_OK_NODATA) {
        klog_note(KLOG_DESKTOP, "[gpu] command 0x%x refused (response 0x%x)\n",
                  ((const gpu_ctrl_hdr_t *)req)->type, resp.type);
        return false;
    }
    return true;
}

bool virtio_gpu_init(void) {
    virtio_dev_t dev;
    if (!virtio_open(&dev, VIRTIO_GPU_DEVICE_ID)) {
        klog_note(KLOG_DESKTOP, "[gpu] no virtio-gpu device found\n");
        return false;
    }
    if (!virtio_setup_queue(&dev, 0, 64, &g_ctrl)) {
        klog_note(KLOG_DESKTOP, "[gpu] could not set up the control queue\n");
        return false;
    }
    g_queue_ready = true;
    virtio_driver_ok(&dev);

    gpu_ctrl_hdr_t req;
    memset(&req, 0, sizeof req);
    req.type = CMD_GET_DISPLAY_INFO;
    gpu_display_info_t resp;
    if (!virtio_submit(&g_ctrl, &req, sizeof req, &resp, sizeof resp)) {
        klog_note(KLOG_DESKTOP, "[gpu] GET_DISPLAY_INFO got no answer\n");
        return false;
    }
    if (resp.hdr.type != RESP_OK_DISPLAY_INFO) {
        klog_note(KLOG_DESKTOP, "[gpu] GET_DISPLAY_INFO failed (response 0x%x)\n", resp.hdr.type);
        return false;
    }

    /* The device configuration says how many scanouts it has (at offset 8). */
    uint32_t num = dev.device != NULL ? *(volatile uint32_t *)(dev.device + 8) : 1u;
    if (num < 1u) num = 1u;
    if (num > VIRTIO_GPU_MAX_SCANOUTS) num = VIRTIO_GPU_MAX_SCANOUTS;
    g_gpu.count = num;
    for (uint32_t i = 0; i < num; i++) {
        g_gpu.width[i] = resp.pmodes[i].width;
        g_gpu.height[i] = resp.pmodes[i].height;
        g_gpu.enabled[i] = resp.pmodes[i].enabled != 0;
        if (g_gpu.enabled[i]) klog_note(KLOG_DESKTOP, "[gpu] scanout %u: %ux%u\n", i,
                                        g_gpu.width[i], g_gpu.height[i]);
    }
    g_gpu.ready = true;
    return true;
}

uint32_t virtio_gpu_scanout_count(void) {
    return g_gpu.ready ? g_gpu.count : 0u;
}

bool virtio_gpu_scanout(uint32_t index, uint32_t *width, uint32_t *height, bool *enabled) {
    if (!g_gpu.ready || index >= g_gpu.count) return false;
    if (width != NULL) *width = g_gpu.width[index];
    if (height != NULL) *height = g_gpu.height[index];
    if (enabled != NULL) *enabled = g_gpu.enabled[index];
    return true;
}

bool virtio_gpu_display_init(void) {
    if (!g_gpu.ready || !g_gpu.enabled[0]) return false;
    uint32_t w = g_gpu.width[0], h = g_gpu.height[0];
    if (w == 0 || h == 0) return false;

    uint64_t bytes = (uint64_t)w * h * 4u;
    size_t pages = (size_t)((bytes + 4095u) / 4096u);
    uintptr_t phys = pmm_alloc_frames(pages);          /* one run: the device reads it by DMA */
    if (phys == 0) {
        klog_note(KLOG_DESKTOP, "[gpu] no contiguous %u KiB for the display\n", (unsigned)(bytes / 1024u));
        return false;
    }
    uint8_t *pixels = (uint8_t *)vmm_phys_to_virt(phys);
    memset(pixels, 0, (size_t)bytes);

    gpu_create_2d_t create;
    memset(&create, 0, sizeof create);
    create.hdr.type = CMD_RESOURCE_CREATE_2D;
    create.resource_id = DISPLAY_RESOURCE;
    create.format = FORMAT_B8G8R8X8_UNORM;
    create.width = w;
    create.height = h;
    if (!command_ok(&create, sizeof create)) return false;

    gpu_attach_t attach;
    memset(&attach, 0, sizeof attach);
    attach.hdr.type = CMD_RESOURCE_ATTACH_BACKING;
    attach.resource_id = DISPLAY_RESOURCE;
    attach.nr_entries = 1;
    attach.addr = phys;
    attach.length = (uint32_t)bytes;
    if (!command_ok(&attach, sizeof attach)) return false;

    gpu_set_scanout_t scan;
    memset(&scan, 0, sizeof scan);
    scan.hdr.type = CMD_SET_SCANOUT;
    scan.r.width = w;
    scan.r.height = h;
    scan.scanout_id = 0;
    scan.resource_id = DISPLAY_RESOURCE;
    if (!command_ok(&scan, sizeof scan)) return false;

    g_disp.pixels = pixels;
    g_disp.phys = phys;
    g_disp.width = w;
    g_disp.height = h;
    g_disp.pitch = w * 4u;
    g_disp.ready = true;
    klog_note(KLOG_DESKTOP, "[gpu] display %ux%u on scanout 0 (resource %u)\n", w, h, DISPLAY_RESOURCE);
    return true;
}

bool virtio_gpu_backing(void **pixels, uint32_t *width, uint32_t *height, uint32_t *pitch) {
    if (!g_disp.ready) return false;
    if (pixels != NULL) *pixels = g_disp.pixels;
    if (width != NULL) *width = g_disp.width;
    if (height != NULL) *height = g_disp.height;
    if (pitch != NULL) *pitch = g_disp.pitch;
    return true;
}

bool virtio_gpu_present(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (!g_disp.ready || w == 0 || h == 0) return false;
    if ((uint64_t)x + w > g_disp.width || (uint64_t)y + h > g_disp.height) return false;

    gpu_transfer_t t;
    memset(&t, 0, sizeof t);
    t.hdr.type = CMD_TRANSFER_TO_HOST_2D;
    t.r.x = x;
    t.r.y = y;
    t.r.width = w;
    t.r.height = h;
    t.offset = (uint64_t)y * g_disp.pitch + (uint64_t)x * 4u;
    t.resource_id = DISPLAY_RESOURCE;
    if (!command_ok(&t, sizeof t)) return false;

    gpu_flush_t f;
    memset(&f, 0, sizeof f);
    f.hdr.type = CMD_RESOURCE_FLUSH;
    f.r.x = x;
    f.r.y = y;
    f.r.width = w;
    f.r.height = h;
    f.resource_id = DISPLAY_RESOURCE;
    return command_ok(&f, sizeof f);
}
