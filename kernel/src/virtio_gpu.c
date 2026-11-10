/* virtio_gpu.c — the virtio-gpu device, step 1: the display list (see virtio_gpu.h).
 *
 * The control queue carries commands as a header plus a body. GET_DISPLAY_INFO has no
 * body; the response is a header plus one entry per scanout (rectangle, enabled flag). */
#include "virtio_gpu.h"

#include <stddef.h>

#include "klog.h"
#include "kstring.h"
#include "virtio.h"

#define VIRTIO_GPU_DEVICE_ID 0x1050u         /* virtio-gpu, modern (non-transitional) */
#define VIRTIO_GPU_MAX_SCANOUTS 16u

#define CMD_GET_DISPLAY_INFO  0x0100u
#define RESP_OK_DISPLAY_INFO  0x1101u

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
    uint32_t enabled;
    uint32_t flags;
} gpu_display_one_t;                      /* 24 bytes */

typedef struct {
    gpu_ctrl_hdr_t hdr;
    gpu_display_one_t pmodes[VIRTIO_GPU_MAX_SCANOUTS];
} gpu_display_info_t;                     /* 408 bytes */

static struct {
    bool ready;
    uint32_t count;
    uint32_t width[VIRTIO_GPU_MAX_SCANOUTS];
    uint32_t height[VIRTIO_GPU_MAX_SCANOUTS];
    bool enabled[VIRTIO_GPU_MAX_SCANOUTS];
} g_gpu;

bool virtio_gpu_init(void) {
    virtio_dev_t dev;
    if (!virtio_open(&dev, VIRTIO_GPU_DEVICE_ID)) {
        klog_note(KLOG_DESKTOP, "[gpu] no virtio-gpu device found\n");
        return false;
    }
    virtq_t ctrl;
    if (!virtio_setup_queue(&dev, 0, 64, &ctrl)) {
        klog_note(KLOG_DESKTOP, "[gpu] could not set up the control queue\n");
        return false;
    }
    virtio_driver_ok(&dev);

    gpu_ctrl_hdr_t req;
    memset(&req, 0, sizeof req);
    req.type = CMD_GET_DISPLAY_INFO;
    gpu_display_info_t resp;
    if (!virtio_submit(&ctrl, &req, sizeof req, &resp, sizeof resp)) {
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
