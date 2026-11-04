/* mac_appkit_x11.h — Cocoa/AppKit translation layer to Helix X11 server.
 *
 * Bridges Mac GUI applications to Helix's X11 server implementation.
 * Provides macOS window styling (traffic lights, titlebar, menubar)
 * and event translation.
 *
 * Part of Helix Mac/Darwin compatibility subsystem.
 */

#ifndef MAC_DARWIN_APPKIT_X11_H
#define MAC_DARWIN_APPKIT_X11_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Standard macOS Colors (ARGB 32-bit) */
#define MAC_COLOR_TITLEBAR          0xFFECECEC
#define MAC_COLOR_TITLEBAR_INACTIVE 0xFFF6F6F6
#define MAC_COLOR_WINDOW_BG         0xFFFFFFFF
#define MAC_COLOR_WINDOW_BORDER     0xFFBDBDBD
#define MAC_COLOR_TEXT_PRIMARY      0xFF1D1D1F
#define MAC_COLOR_TEXT_SECONDARY    0xFF86868B
#define MAC_COLOR_MENUBAR_BG        0xFFE8E8E8
#define MAC_COLOR_HIGHLIGHT_BLUE    0xFF007AFF

/* Traffic Light Button Colors */
#define MAC_COLOR_TRAFFIC_RED       0xFFFF5F56  /* Close */
#define MAC_COLOR_TRAFFIC_YELLOW    0xFFFFBD2E  /* Minimize */
#define MAC_COLOR_TRAFFIC_GREEN     0xFF27C93F  /* Zoom */
#define MAC_COLOR_TRAFFIC_BORDER    0xFFD64540

/* Traffic Light Button Hit Targets */
#define MAC_TRAFFIC_LIGHT_RADIUS    6
#define MAC_TRAFFIC_RED_X           18
#define MAC_TRAFFIC_YELLOW_X        38
#define MAC_TRAFFIC_GREEN_X         58
#define MAC_TRAFFIC_Y               15

/* Event Types (NSEvent emulation) */
typedef enum {
    NS_EVENT_NONE = 0,
    NS_EVENT_LEFT_MOUSE_DOWN,
    NS_EVENT_LEFT_MOUSE_UP,
    NS_EVENT_MOUSE_MOVED,
    NS_EVENT_KEY_DOWN,
    NS_EVENT_KEY_UP,
    NS_EVENT_WINDOW_EXPOSE,
    NS_EVENT_WINDOW_CLOSE,
} ns_event_type_t;

typedef struct {
    ns_event_type_t type;
    int16_t         x;
    int16_t         y;
    uint32_t        keycode;
    char            character;
    uint32_t        modifier_flags;
} ns_event_t;

/* AppKit Control Types */
typedef enum {
    NS_CONTROL_VIEW = 0,
    NS_CONTROL_BUTTON,
    NS_CONTROL_LABEL,
    NS_CONTROL_TEXT_FIELD,
    NS_CONTROL_MENU_BAR,
} ns_control_type_t;

/* Forward declaration */
typedef struct ns_window ns_window_t;
typedef struct ns_view ns_view_t;

typedef void (*ns_action_callback_t)(ns_view_t *sender, void *context);

/* Cocoa View */
struct ns_view {
    uint32_t            id;
    ns_control_type_t   type;
    int16_t             x, y;
    uint16_t            width, height;
    char                title[128];
    uint32_t            bg_color;
    uint32_t            text_color;
    bool                is_highlighted;
    ns_action_callback_t on_click;
    void               *context;
    struct ns_view     *next;
};

/* Cocoa Window */
struct ns_window {
    uint32_t       x11_window_id;
    int16_t        x, y;
    uint16_t       width, height;
    char           title[128];
    uint32_t       style_mask;
    bool           is_visible;
    bool           is_key_window;
    ns_view_t     *views;
    uint32_t       view_count;
    struct ns_window *next;
};

/* AppKit Application State */
typedef struct {
    int          x11_fd;
    uint32_t     display_width;
    uint32_t     display_height;
    ns_window_t *windows;
    uint32_t     next_window_id;
    bool         is_running;
} ns_application_t;

extern ns_application_t g_ns_app;

/* AppKit / Cocoa API */
int           ns_application_init(const char *display_name);
void          ns_application_run(void);
void          ns_application_terminate(void);

ns_window_t  *ns_window_create(int16_t x, int16_t y, uint16_t width, uint16_t height, const char *title);
void          ns_window_order_front(ns_window_t *win);
void          ns_window_close(ns_window_t *win);
void          ns_window_draw(ns_window_t *win);

ns_view_t    *ns_button_create(int16_t x, int16_t y, uint16_t w, uint16_t h, const char *title,
                               ns_action_callback_t callback, void *context);
ns_view_t    *ns_label_create(int16_t x, int16_t y, uint16_t w, uint16_t h, const char *text);
void          ns_window_add_subview(ns_window_t *win, ns_view_t *view);

/* Event & Hit Testing */
int           ns_window_hit_test_traffic_lights(const ns_window_t *win, int16_t mouse_x, int16_t mouse_y);
void          ns_dispatch_event(const ns_event_t *ev);
void          ns_draw_mac_window_frame(ns_window_t *win);

/* X11 Protocol Bridge Helpers */
int           x11_bridge_send_create_window(int fd, uint32_t win_id, int16_t x, int16_t y, uint16_t w, uint16_t h);
int           x11_bridge_send_map_window(int fd, uint32_t win_id);
int           x11_bridge_send_fill_rect(int fd, uint32_t win_id, int16_t x, int16_t y, uint16_t w, uint16_t h, uint32_t color);

#ifdef __cplusplus
}
#endif

#endif /* MAC_DARWIN_APPKIT_X11_H */
