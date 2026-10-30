/* display_surface.h — display surface object for Helix OS.
 *
 * M12 milestone: graphics.
 *
 * Defines a display surface object that wraps the framebuffer. Display
 * surfaces can be created with dimensions, resolution, and pixel formats.
 * The framebuffer is already initialized during boot.
 *
 * This provides a foundation for a compositor and graphics pipeline.
 */

#ifndef HELIX_DISPLAY_SURFACE_H
#define HELIX_DISPLAY_SURFACE_H

#include "object.h"

typedef struct display_surface display_surface_t;

struct display_surface {
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
    uint32_t pitch;
    void *framebuffer;
    char name[32];
};

void display_surface_init(display_surface_t *ds, const char *name,
                         uint32_t width, uint32_t height,
                         uint32_t bpp, uint32_t pitch, void *fb);
object_t *display_surface_create(void);

#endif /* HELIX_DISPLAY_SURFACE_H */