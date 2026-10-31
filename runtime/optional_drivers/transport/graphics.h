/* transport/graphics.h — Helix transport adapter for Linux graphics drivers.
 *
 * Defines minimal types for Linux's DRM subsystem: struct drm_device,
 * struct drm_framebuffer, etc. This lets Mesa drivers (mesa-dri, panfrost)
 * be compiled alongside Helix's existing display-surface framework (M12),
 * which provides a native rendering surface model. The adapter exposes the
 * Linux driver's rendering pipeline as Helix's native framebuffer callbacks.
 */

#ifndef HELIX_TRANSPORT_GRAPHICS_H
#define HELIX_TRANSPORT_GRAPHICS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -- Display dimensions ---------------------------------------------------- */

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t bits_per_pixel;  /* e.g. 32, 24 */
} display_size_t;

/* -- Framebuffer types ----------------------------------------------------- */

#define DRM_FORMAT_RGB888 0x33324742ULL
#define DRM_FORMAT_XRGB8888 0x34324742ULL
#define DRM_FORMAT_ARGB8888 0x34324742ULL

typedef struct {
    uint32_t format;
    display_size_t size;
    void *mmap_ptr;            /* Mapped GPU memory */
    uint32_t pitch;            /* Bytes per row */
    uint32_t handle;           /* GEM handle */
} drm_fb_t;

/* -- Graphics context ------------------------------------------------------ */

typedef struct {
    drm_fb_t primary;         /* Primary display buffer */
    drm_fb_t cursor;          /* Software cursor */
    uint32_t width;
    uint32_t height;
    uint32_t bits_per_pixel;  /* e.g. 32, 24 */
    bool swapchain_enabled;    /* Double buffering */
} graphics_context_t;

/* -- Transport adapter functions ------------------------------------------- */

/* Open a graphics context for a device */
int transport_graphics_open(const char *name, graphics_context_t *ctx);

/* Register display surface with Helix object manager */
int transport_graphics_register_display(graphics_context_t *ctx);

/* Flush and present a framebuffer */
int transport_graphics_flush(graphics_context_t *ctx, const drm_fb_t *fb);

/* Clash and close a graphics context */
void transport_graphics_close(graphics_context_t *ctx);

/* NVK/Zinc (NVIDIA) support: register an accelerated rendering context.
 * Returns -1 if unsupported, 0 on success. */
int transport_nvk_zinc_register(const char *name, uint32_t vendor_id);

#ifdef __cplusplus
}
#endif

#endif /* HELIX_TRANSPORT_GRAPHICS_H */