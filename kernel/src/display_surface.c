/* display_surface.c — display surface object implementation for Helix OS.
 *
 * M12 milestone: graphics.
 *
 * Provides a display surface object that wraps the framebuffer.
 */

#include "display_surface.h"
#include "console.h"
#include "heap.h"
#include "kstring.h"

#define MAX_DISPLAY_SURFACES 4

static display_surface_t *g_display_surfaces[MAX_DISPLAY_SURFACES];
static uint8_t g_display_surface_count;

void display_surface_init(display_surface_t *ds, const char *name,
                         uint32_t width, uint32_t height,
                         uint32_t bpp, uint32_t pitch, void *fb) {
    strcpy(ds->name, name);
    ds->width = width;
    ds->height = height;
    ds->bpp = bpp;
    ds->pitch = pitch;
    ds->framebuffer = fb;
}

object_t *display_surface_create(void) {
    if (g_display_surface_count >= MAX_DISPLAY_SURFACES) return NULL;

    display_surface_t *ds = kzalloc(sizeof(*ds));
    if (ds == NULL) return NULL;

    ds->width = 1280;
    ds->height = 800;
    ds->bpp = 32;
    ds->pitch = ds->width * 4;

    g_display_surfaces[g_display_surface_count++] = ds;
    return NULL;  /* Placeholder for now */
}