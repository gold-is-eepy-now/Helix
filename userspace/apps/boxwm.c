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
#include <pthread.h>

#include "linux_runtime.h"
#include "../runtime/optional_drivers/transport/graphics.h"

#define MAX_WINDOWS 64

/* Window state */
typedef struct {
    uint32_t id;
    int16_t x, y;
    uint16_t width, height;
    uint32_t *pixels;
    uint32_t *framebuffer;
    bool mapped;
    bool focused;
    bool dirty;
    uint32_t *window_pixels;  /* Window-specific pixel buffer */
    uint16_t win_width, win_height;
    uint32_t *window_base;    /* Base address in framebuffer */
} window_t;

/* Global state */
static window_t g_windows[MAX_WINDOWS];
static uint32_t g_next_window_id = 1;
static window_t *g_focused_window = NULL;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Window operations */
static window_t *window_create(uint16_t width, uint16_t height) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!g_windows[i].mapped) {
            window_t *w = &g_windows[i];
            w->pixels = calloc(width * height, sizeof(uint32_t));
            if (!w->pixels) return NULL;
            
            w->width = width;
            w->height = height;
            w->mapped = false;
            w->focused = false;
            w->dirty = false;
            w->window_pixels = NULL;
            w->window_base = NULL;
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
    free(window->window_pixels);
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

/* Render window to framebuffer */
static void window_render(window_t *window) {
    if (!window || !window->mapped) return;
    
    if (window->dirty) {
        /* Copy window pixels to framebuffer */
        for (uint16_t y = 0; y < window->height; y++) {
            for (uint16_t x = 0; x < window->width; x++) {
                uint32_t px = (window->y + y) * window->width + (window->x + x);
                window->framebuffer[px] = window->pixels[y * window->width + x];
            }
        }
        window->dirty = false;
    }
}

static void *window_thread(void *arg) {
    window_t *window = (window_t *)arg;
    
    while (1) {
        /* Simulate window manager work */
        sleep(1);
        
        /* In real implementation this would process events */
        if (window->dirty) {
            window_render(window);
            window->dirty = false;
        }
    }
    
    return NULL;
}

int main(int argc, char **argv) {
    printf("boxwm: Starting window manager...\n");
    
    /* Initialize X11 */
    /* In real implementation, this would initialize the X server */
    
    /* Create test windows */
    window_t *win1 = window_create(800, 600);
    window_t *win2 = window_create(400, 400);
    
    if (win1) window_map(win1);
    if (win2) window_map(win2);
    
    /* Set focus */
    window_focus(win1);
    
    /* Start window threads */
    pthread_t t1, t2;
    pthread_create(&t1, NULL, window_thread, win1);
    pthread_create(&t2, NULL, window_thread, win2);
    
    /* Main loop */
    while (1) {
        /* Simulate window manager events */
        sleep(1);
        
        /* Example: toggle focus */
        if (g_focused_window) {
            window_t *old = g_focused_window;
            window_focus(win2);
            window_focus(old);
        }
    }
    
    return 0;
}