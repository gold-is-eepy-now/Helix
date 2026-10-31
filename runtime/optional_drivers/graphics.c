/* graphics.c — Helix transport adapter for Linux graphics drivers.
 *
 * This file provides the minimal translation layer that allows Linux's
 * Mesa/DRM driver model to work with Helix's native object framework.
 *
 * It implements a stub that adapts the Linux DRM structure to Helix's
 * display-surface framework. The actual driver logic is expected to be
 * compiled with this header, allowing Mesa drivers and NVK/Zinc to be
 * built as optional components.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include "transport/graphics.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -- Graphics context state ------------------------------------------------ */

typedef struct {
    graphics_context_t *ctx;
    bool initialized;
    bool active;
} graphics_dev_t;

/* -- Transport adapter functions ------------------------------------------- */

/* Open a graphics context for a device */
int transport_graphics_open(const char *name, graphics_context_t *ctx) {
    if (!ctx) return -1;
    
    graphics_dev_t *dev = (graphics_dev_t *)malloc(sizeof(graphics_dev_t));
    if (!dev) return -1;
    
    dev->ctx = (graphics_context_t *)malloc(sizeof(graphics_context_t));
    if (!dev->ctx) {
        free(dev);
        return -1;
    }
    
    memset(dev->ctx, 0, sizeof(graphics_context_t));
    dev->ctx->width = 1920;
    dev->ctx->height = 1080;
    dev->ctx->bits_per_pixel = 32;
    dev->ctx->swapchain_enabled = true;
    dev->initialized = true;
    dev->active = true;
    
    return 0;
}

/* Register display surface with Helix object manager */
int transport_graphics_register_display(graphics_context_t *ctx) {
    /* In a real implementation, this would register with the object manager */
    return 0;
}

/* Flush and present a framebuffer */
int transport_graphics_flush(graphics_context_t *ctx, const drm_fb_t *fb) {
    if (!ctx || !fb) return -1;
    /* In a real implementation, this would present the framebuffer */
    return 0;
}

/* Clash and close a graphics context */
void transport_graphics_close(graphics_context_t *ctx) {
    if (ctx) {
        free(ctx);
    }
    return;
}

/* NVK/Zinc (NVIDIA) support: register an accelerated rendering context. */
int transport_nvk_zinc_register(const char *name, uint32_t vendor_id) {
    if (vendor_id == 0x10DE) {  /* NVIDIA vendor ID */
        return 0;
    }
    return -1;
}

#ifdef __cplusplus
}
#endif