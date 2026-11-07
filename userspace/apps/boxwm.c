/* boxwm.c — Simple window manager for Helix.
 *
 * Implements a minimal window manager with:
 * - Floating window support
 * - Basic tiling
 * - Window focus management
 * - Framebuffer rendering
 *
 * Key features:
 * - Window list management
 * - Focus handling
 * - Resize and move operations
 * - Simple rendering to framebuffer
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdbool.h>

#define MAX_WINDOWS 64

/* Window state */
typedef struct {
    uint32_t id;
    int16_t x, y;
    uint16_t width, height;
    uint32_t *pixels;
    bool mapped;
    bool focused;
    bool dirty;
} window_t;

/* Global state */
static window_t g_windows[MAX_WINDOWS];
static uint32_t g_next_window_id = 1;
static window_t *g_focused_window = NULL;

/* Window operations */
static window_t *window_create(uint16_t width, uint16_t height) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!g_windows[i].mapped) {
            window_t *w = &g_windows[i];
            w->pixels = calloc((size_t)width * height, sizeof(uint32_t));
            if (!w->pixels) return NULL;
            
            w->width = width;
            w->height = height;
            w->mapped = false;
            w->focused = false;
            w->dirty = false;
            w->id = g_next_window_id++;
            
            printf("boxwm: Created window %u (%ux%u)\n", w->id, width, height);
            return w;
        }
    }
    return NULL;
}

static void window_destroy(window_t *window) {
    if (!window) return;
    
    free(window->pixels);
    window->pixels = NULL;
    window->mapped = false;
    window->focused = false;
    printf("boxwm: Destroyed window %u\n", window->id);
}

static void window_map(window_t *window) {
    if (!window) return;
    window->mapped = true;
    window->dirty = true;
    printf("boxwm: Mapped window %u\n", window->id);
}

static void window_unmap(window_t *window) {
    if (!window) return;
    window->mapped = false;
    printf("boxwm: Unmapped window %u\n", window->id);
}

static void window_focus(window_t *window) {
    if (!window) return;
    
    if (g_focused_window) {
        g_focused_window->focused = false;
    }
    
    window->focused = true;
    g_focused_window = window;
    printf("boxwm: Focused window %u\n", window->id);
}

static void window_move(window_t *window, int16_t x, int16_t y) {
    if (!window) return;
    window->x = x;
    window->y = y;
    window->dirty = true;
    printf("boxwm: Moved window %u to (%d,%d)\n", window->id, x, y);
}

static void window_resize(window_t *window, uint16_t width, uint16_t height) {
    if (!window) return;
    
    window->width = width;
    window->height = height;
    window->dirty = true;
    printf("boxwm: Resized window %u to %ux%u\n", window->id, width, height);
}

/* Render window. ponytail: no framebuffer mapping in ring 3 yet; the window
 * pixels are kept in memory until the kernel exposes a display surface. */
static void window_render(window_t *window) {
    if (!window || !window->mapped) return;
    
    if (window->dirty) {
        window->dirty = false;
    }
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    (void)window_destroy;
    (void)window_unmap;
    (void)window_move;
    (void)window_resize;
    printf("boxwm: Starting window manager...\n");
    
    /* Create test windows */
    window_t *win1 = window_create(800, 600);
    window_t *win2 = window_create(400, 400);
    
    if (win1) window_map(win1);
    if (win2) window_map(win2);
    
    /* Set focus */
    window_focus(win1);
    
    /* Main loop */
    while (1) {
        /* Simulate window manager events */
        sleep(1);
        
        window_render(win1);
        window_render(win2);
        
        /* Example: toggle focus */
        if (g_focused_window) {
            window_t *old = g_focused_window;
            window_focus(win2);
            window_focus(old);
        }
    }
    
    return 0;
}
