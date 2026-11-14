/* desktop.c — a basic Windows 98-style desktop, drawn on the framebuffer.
 *
 * What it is: a teal desktop with two icons, a grey taskbar with a Start menu
 * and a clock, and bevelled windows with navy title bars. "My Computer" is an
 * Explorer-style window over the ext4 volume (folders, files, a preview of a file);
 * "Settings" shows read-only system information; "About Helix" is static text.
 *
 * Terminal icon: a window that runs kernel shell commands (the recovery shell) and
 * exec <path> for ring-3 programs, with their output shown in the window.
 *
 * Mouse: a PS/2 or USB mouse moves an arrow cursor. Moving it only redraws the
 * arrow (the pixels under it are saved and restored); the desktop repaints on
 * window and menu changes. A left click acts on whatever is under the pointer:
 * an icon opens its window, Start toggles the menu, a menu item runs, a title-bar
 * x closes a window, clicking a window or taskbar button focuses it, and dragging
 * a title bar moves an outline; the window is redrawn on release.
 *
 * What it is not (yet): the desktop runs in the kernel, not as a ring-3 program,
 * and there is no double-click or right-click.
 *
 * Keys (in My Computer): Up/Down select, Enter opens, Backspace goes up,
 *        n new folder, f new file, c copy, e edit, d delete (press twice).
 * Keys (in Network): type a host name or address, Enter pings it.
 * Keys (in Settings): t toggles the light and dark theme.
 * Keys:  Up/Down  move the icon selection (or the Start menu selection)
 *        Enter    open the selected icon or menu item
 *        s        open or close the Start menu
 *        Tab      switch to the next window
 *        x        close the focused window
 *        Esc      close the Start menu, or leave the desktop when it is closed
 */
#include "desktop.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "console.h"
#include "editor.h"
#include "ext4.h"
#include "fb.h"
#include "icon_pack.h"
#include "input.h"
#include "login.h"
#include "ui.h"
#include "keyboard.h"
#include "accounts.h"
#include "klog.h"
#include "html_text.h"
#include "version.h"
#include "net_http.h"
#include "net.h"
#include "net_dns.h"
#include "net_icmp.h"
#include "ps2_mouse.h"
#include "kstring.h"
#include "panic.h"
#include "shell.h"
#include "time.h"
#include "usb_host.h"
#include "usb_hid.h"

/* Win98-style palette, packed for the framebuffer at run time. */
static uint32_t c_desk, c_face, c_white, c_dark, c_black, c_navy, c_gray;
static uint32_t c_client, c_ink;      /* client-area fill and text on it: set by theme_apply */

/* The theme: every colour that differs between light and dark, as 0xRRGGBB. The packed
 * c_* globals above are set from the current theme by theme_apply(); the window, taskbar,
 * menu and wallpaper drawing reads these fields directly. */
typedef struct {
    uint32_t client, ink, muted, heading, face, edge;
    uint32_t frame_rim, frame_focus_top, frame_focus_bot, frame_blur_top, frame_blur_bot;
    uint32_t cap_focus, cap_blur, client_edge;
    uint32_t desk_top, desk_bot;
    uint32_t task_top, task_bot, task_line;
    uint32_t tbtn_focus_top, tbtn_focus_bot, tbtn_focus_text, tbtn_blur_top, tbtn_blur_bot;
    uint32_t menu_rim, menu_top, menu_bot, menu_strip, menu_hi_top, menu_hi_bot;
    uint32_t menu_hi_text;      /* text on a highlighted Start menu row */
    uint32_t track, thumb_top, thumb_bot;
} theme_t;

/* The light theme is the Win98 and Windows 7 look the desktop had before themes. */
static const theme_t g_theme_light = {
    .client = 0xFFFFFFu, .ink = 0x000000u, .muted = 0x808080u, .heading = 0x000080u,
    .face = 0xC0C0C0u, .edge = 0x808080u,
    .frame_rim = 0x1E4C86u, .frame_focus_top = 0xC4DDF5u, .frame_focus_bot = 0x7FA9D6u,
    .frame_blur_top = 0xE4ECF5u, .frame_blur_bot = 0xB6C9DEu,
    .cap_focus = 0x10233Du, .cap_blur = 0x4D6480u, .client_edge = 0x5A7AA3u,
    .desk_top = 0x6FB6F0u, .desk_bot = 0x0D3D78u,
    .task_top = 0x33507Au, .task_bot = 0x0F1E36u, .task_line = 0x8FA9CCu,
    .tbtn_focus_top = 0xA7CCF0u, .tbtn_focus_bot = 0x5B8CC6u, .tbtn_focus_text = 0x10233Du,
    .tbtn_blur_top = 0x4D6E9Cu, .tbtn_blur_bot = 0x2C4870u,
    .menu_rim = 0x2B4C7Eu, .menu_top = 0xF4F8FDu, .menu_bot = 0xDCE7F4u, .menu_strip = 0x1B3157u,
    .menu_hi_top = 0xA7CCF0u, .menu_hi_bot = 0x6FA2DAu, .menu_hi_text = 0x10233Du,
    .track = 0xDDE4EEu, .thumb_top = 0xB8D6F2u, .thumb_bot = 0x5F93CFu,
};

/* The dark theme: dark glass and client areas, light text, and blues kept for accents. */
static const theme_t g_theme_dark = {
    .client = 0x1E2430u, .ink = 0xE6EAF0u, .muted = 0x8A94A6u, .heading = 0x7FB2FFu,
    .face = 0x2C323Du, .edge = 0x4A5566u,
    .frame_rim = 0x0A1220u, .frame_focus_top = 0x2D4A6Eu, .frame_focus_bot = 0x1A2F4Du,
    .frame_blur_top = 0x27364Du, .frame_blur_bot = 0x18222Fu,
    .cap_focus = 0xE6EEF8u, .cap_blur = 0x9AAABDu, .client_edge = 0x3A4A62u,
    .desk_top = 0x1B2A44u, .desk_bot = 0x06101Fu,
    .task_top = 0x1F2B3Du, .task_bot = 0x0B1220u, .task_line = 0x3A4C66u,
    .tbtn_focus_top = 0x3D6FB0u, .tbtn_focus_bot = 0x274E82u, .tbtn_focus_text = 0xE6EEF8u,
    .tbtn_blur_top = 0x2A3A52u, .tbtn_blur_bot = 0x1B2638u,
    .menu_rim = 0x0A1220u, .menu_top = 0x1E2633u, .menu_bot = 0x161C27u, .menu_strip = 0x0F2440u,
    .menu_hi_top = 0x35598Au, .menu_hi_bot = 0x274775u, .menu_hi_text = 0xE6EEF8u,
    .track = 0x2C323Du, .thumb_top = 0x4A6A96u, .thumb_bot = 0x33507Au,
};

/* Helix Aero Blue: the default. Glass with a brighter sky-blue desktop and a lighter taskbar
 * than Helix Light, so the title bars and taskbar read as glossy blue. The values are new and
 * were chosen by hand; they have not been compared with Windows 7 pixels. */
static const theme_t g_theme_aero_blue = {
    .client = 0xFFFFFFu, .ink = 0x000000u, .muted = 0x6A7A8Cu, .heading = 0x1B4F8Au,
    .face = 0xD9E4F1u, .edge = 0x7F98B5u,
    .frame_rim = 0x1E4C86u, .frame_focus_top = 0xD2E6FAu, .frame_focus_bot = 0x6FA0D6u,
    .frame_blur_top = 0xEAF1F9u, .frame_blur_bot = 0xBCD0E6u,
    .cap_focus = 0x10233Du, .cap_blur = 0x4D6480u, .client_edge = 0x5A7AA3u,
    .desk_top = 0x8FD0FFu, .desk_bot = 0x124E96u,
    .task_top = 0x4C7AB0u, .task_bot = 0x163A6Cu, .task_line = 0xA9C6E6u,
    .tbtn_focus_top = 0xBFDDF8u, .tbtn_focus_bot = 0x5F95D2u, .tbtn_focus_text = 0x10233Du,
    .tbtn_blur_top = 0x5E84B5u, .tbtn_blur_bot = 0x345E94u,
    .menu_rim = 0x2B4C7Eu, .menu_top = 0xF6FAFFu, .menu_bot = 0xDDE9F7u, .menu_strip = 0x1B3157u,
    .menu_hi_top = 0xBFDDF8u, .menu_hi_bot = 0x6FA2DAu, .menu_hi_text = 0x10233Du,
    .track = 0xDDE4EEu, .thumb_top = 0xB8D6F2u, .thumb_bot = 0x5F93CFu,
};

/* Helix Aero Dark: the dark glass with blue accents, so it still reads as the Aero family. */
static const theme_t g_theme_aero_dark = {
    .client = 0x1B2330u, .ink = 0xE6EEF8u, .muted = 0x8FA3BD, .heading = 0x8CC4FFu,
    .face = 0x27334Au, .edge = 0x40557Au,
    .frame_rim = 0x081A36u, .frame_focus_top = 0x2F5C94u, .frame_focus_bot = 0x163767u,
    .frame_blur_top = 0x22324Du, .frame_blur_bot = 0x16233Au,
    .cap_focus = 0xEAF3FFu, .cap_blur = 0x9FB4CFu, .client_edge = 0x34507Cu,
    .desk_top = 0x1E4C8Au, .desk_bot = 0x061C3Eu,
    .task_top = 0x24406Cu, .task_bot = 0x0A1B36u, .task_line = 0x3E5F8Eu,
    .tbtn_focus_top = 0x4B82C4u, .tbtn_focus_bot = 0x2A5A98u, .tbtn_focus_text = 0xEAF3FFu,
    .tbtn_blur_top = 0x2E4D7Au, .tbtn_blur_bot = 0x1B3257u,
    .menu_rim = 0x081A36u, .menu_top = 0x1F2B40u, .menu_bot = 0x18233Au, .menu_strip = 0x0C2A52u,
    .menu_hi_top = 0x3D6AA5u, .menu_hi_bot = 0x2A5490u, .menu_hi_text = 0xEAF3FFu,
    .track = 0x27334Au, .thumb_top = 0x4A6A96u, .thumb_bot = 0x33507Au,
};

/* Helix Classic: flat Windows 9x grey and navy, no gradients. Every top and bottom pair is
 * the same colour, so the glass drawing code just draws solid fills. */
static const theme_t g_theme_classic = {
    .client = 0xFFFFFFu, .ink = 0x000000u, .muted = 0x808080u, .heading = 0x000080u,
    .face = 0xC0C0C0u, .edge = 0x808080u,
    .frame_rim = 0x000000u, .frame_focus_top = 0x000080u, .frame_focus_bot = 0x000080u,
    .frame_blur_top = 0x808080u, .frame_blur_bot = 0x808080u,
    .cap_focus = 0xFFFFFFu, .cap_blur = 0xC0C0C0u, .client_edge = 0x808080u,
    .desk_top = 0x008080u, .desk_bot = 0x008080u,
    .task_top = 0xC0C0C0u, .task_bot = 0xC0C0C0u, .task_line = 0xFFFFFFu,
    .tbtn_focus_top = 0xC0C0C0u, .tbtn_focus_bot = 0xC0C0C0u, .tbtn_focus_text = 0x000000u,
    .tbtn_blur_top = 0xC0C0C0u, .tbtn_blur_bot = 0xC0C0C0u,
    .menu_rim = 0x000000u, .menu_top = 0xC0C0C0u, .menu_bot = 0xC0C0C0u, .menu_strip = 0x000080u,
    .menu_hi_top = 0x000080u, .menu_hi_bot = 0x000080u, .menu_hi_text = 0xFFFFFFu,
    .track = 0xC0C0C0u, .thumb_top = 0xC0C0C0u, .thumb_bot = 0xC0C0C0u,
};

/* The themes in the order Settings cycles through them. Aero Blue is the default. */
#define THEME_COUNT 5u
static const theme_t *const g_theme_list[THEME_COUNT] = {
    &g_theme_aero_blue, &g_theme_aero_dark, &g_theme_classic, &g_theme_light, &g_theme_dark,
};
static const char *const g_theme_names[THEME_COUNT] = {
    "Helix Aero Blue", "Helix Aero Dark", "Helix Classic", "Helix Light", "Helix Dark",
};

static unsigned g_theme_idx;          /* Settings > Appearance; in memory, so Aero Blue at each boot */
static const theme_t *g_th = &g_theme_aero_blue;

/* A 0xRRGGBB colour, packed for the framebuffer. */
static uint32_t rgb_hex(uint32_t v) {
    return fb_rgb((uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v);
}

/* Make the current theme's colours the ones the drawing code uses. */
static void theme_apply(void) {
    g_th = g_theme_list[g_theme_idx];
    c_client = rgb_hex(g_th->client);
    c_ink = rgb_hex(g_th->ink);
    c_face = rgb_hex(g_th->face);
    c_dark = rgb_hex(g_th->edge);
    c_navy = rgb_hex(g_th->heading);
    c_gray = rgb_hex(g_th->muted);
    c_desk = rgb_hex(g_th->desk_top);
}

typedef enum { APP_MYCOMP, APP_ABOUT, APP_SHUTDOWN, APP_TERMINAL, APP_EDITOR, APP_BROWSER, APP_SETTINGS, APP_NETWORK, APP_COUNT } app_t;

static const char *const g_app_names[APP_COUNT] = { "My Computer", "About Helix", "Shut Down", "Terminal", "Text Editor", "Browser", "Settings", "Network" };

#define ICON_COUNT 5            /* icons on the desktop: My Computer, About Helix, Terminal, Text Editor, Browser */
#define MAX_WIN 4
#define MAX_LINES 16
#define LINE_LEN 48
#define TASK_H 36u
#define TB_BTN_X    58u         /* taskbar window buttons: first x, size, step between them */
#define TB_BTN_Y    5u
#define TB_BTN_H    26u
#define TB_BTN_W    150u
#define TB_BTN_STEP 156u
#define HOVER_NONE (-1)         /* what the pointer is over: a window button index, or one of these */
#define HOVER_ORB 10
#define HOVER_SHOW 11           /* Show Desktop strip at the far right of the taskbar */
#define HOVER_MENU 100          /* Start menu rows: HOVER_MENU + column * 50 + row */
#define MENU_W 402u             /* the Start menu panel */
#define MENU_H 320u
#define MENU_ROW_H 20u
#define MENU_ROWS_MAX 16
#define MENU_LEFT_X 4u          /* programs column (search and pinned/recent/all) */
#define MENU_LEFT_CW 192u
#define MENU_RIGHT_X 208u       /* places column */
#define MENU_RIGHT_CW 188u
#define CTX_W 150u              /* the desktop's right-click menu */
#define CTX_ROW 20u
#define CTX_COUNT 3

/* The app each desktop icon opens (icon i is not app i, since Shut Down is an app too). */
static const app_t g_icon_app[ICON_COUNT] = { APP_MYCOMP, APP_ABOUT, APP_TERMINAL, APP_EDITOR, APP_BROWSER };

/* Start menu contents. The left column lists programs: pinned, recently opened, all. The right
 * column lists the places, then Shut Down. Desktop folders for User and Documents do not exist
 * yet, so they are not listed. */
static const app_t g_pinned_apps[4] = { APP_MYCOMP, APP_TERMINAL, APP_EDITOR, APP_BROWSER };
static const app_t g_all_apps[3] = { APP_ABOUT, APP_SETTINGS, APP_NETWORK };
static const app_t g_place_apps[4] = { APP_MYCOMP, APP_NETWORK, APP_SETTINGS, APP_ABOUT };

typedef struct {
    bool used;
    app_t app;
    uint32_t x, y, w, h;
    bool minimised;            /* hidden; its taskbar button restores it */
    bool maximised;            /* fills the screen above the taskbar */
    uint32_t rx, ry, rw, rh;   /* where it goes back to when restored */
} window_t;

static window_t g_win[MAX_WIN];
static editor_t g_ed;          /* the text the Text Editor window shows; one editor, one window */
static int g_focus = -1;
static int g_icon_sel;
static int g_menu_sel;
static bool g_menu_open;
static char g_menu_query[16];   /* typed into the Start menu's search box */
static int g_menu_query_len;
static int g_menu_col;          /* 0 = programs column, 1 = places column (keyboard focus) */
static app_t g_recent[3];       /* recently opened apps, newest first */
static int g_recent_n;
static bool g_ctx_open;         /* the desktop's right-click menu */
static int g_ctx_x, g_ctx_y;
static int g_hover = HOVER_NONE;   /* the taskbar or menu item under the pointer */
static bool g_right_was_down;

/* The pointer position in screen pixels, and the left button from the last poll. */
static int g_mx, g_my;
static bool g_button_was_down;

/* A window being dragged by its title bar (-1 when none), and where inside it the
 * grab happened, so the window keeps the same point under the pointer. */
static int g_drag = -1;
static int g_drag_dx, g_drag_dy;

/* While a title bar is dragged, only an outline follows the pointer, as in Windows 98.
 * The window itself is redrawn once, on release. The outline is drawn by XOR, so
 * drawing it again at the same place erases it exactly and the picture underneath
 * is never lost. */
static int g_out_x, g_out_y;
static bool g_outline_shown;
static int g_resize = -1;          /* window being resized from its bottom-right grip, or -1 */
static int g_rs_w, g_rs_h;         /* the size the resize outline shows */
static int g_last_title_win = -1;  /* a second title-bar click within 500 ms maximises */
static uint64_t g_last_title_ms;
static uint32_t c_outline;

/* What a pointer poll asks the main loop to do. */
enum { PTR_NONE, PTR_REDRAW, PTR_EXIT };

/* Text shown in each application's window, built when the window is opened. */
static char g_text[APP_COUNT][MAX_LINES][LINE_LEN];
static int g_nlines[APP_COUNT];

/* ------------------------------------------------------------------ */
/* Small string helpers (no libc in the kernel)                        */
/* ------------------------------------------------------------------ */

static void put_str(char *dst, size_t cap, const char *src) {
    size_t n = 0;
    while (src[n] != '\0' && n + 1 < cap) {
        dst[n] = src[n];
        n++;
    }
    dst[n] = '\0';
}

static size_t cat_str(char *dst, size_t cap, size_t at, const char *src) {
    while (*src != '\0' && at + 1 < cap) {
        dst[at++] = *src++;
    }
    dst[at] = '\0';
    return at;
}

static size_t cat_dec(char *dst, size_t cap, size_t at, uint64_t v) {
    char tmp[24];
    size_t n = 0;
    do {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0 && n < sizeof(tmp));
    while (n > 0 && at + 1 < cap) {
        dst[at++] = tmp[--n];
    }
    dst[at] = '\0';
    return at;
}

/* ------------------------------------------------------------------ */
/* 3D bevels: a raised or sunken rectangle, as Win98 draws controls.   */
/* ------------------------------------------------------------------ */

static void bevel(uint32_t x, uint32_t y, uint32_t w, uint32_t h, bool raised) {
    uint32_t hi = raised ? c_white : c_dark;
    uint32_t lo = raised ? c_dark : c_white;
    fb_fill_rect(x, y, w, 1, hi);
    fb_fill_rect(x, y, 1, h, hi);
    fb_fill_rect(x, y + h - 1, w, 1, lo);
    fb_fill_rect(x + w - 1, y, 1, h, lo);
}

/* ------------------------------------------------------------------ */
/* Windows 7 look: glass gradients, rounded corners, shadowed text.     */
/* Colours here are 0xRRGGBB and are packed for the framebuffer when   */
/* drawn.                                                              */
/* ------------------------------------------------------------------ */

static uint32_t isqrt_u32(uint32_t n) {
    uint32_t r = 0;
    while ((r + 1) * (r + 1) <= n) r++;
    return r;
}

/* The colour t/n of the way from a to b (both 0xRRGGBB). */
static uint32_t rgb_mix(uint32_t a, uint32_t b, uint32_t t, uint32_t n) {
    if (n == 0) n = 1;
    int out[3];
    for (int i = 0; i < 3; i++) {
        int sa = (int)((a >> (16 - 8 * i)) & 0xFFu);
        int sb = (int)((b >> (16 - 8 * i)) & 0xFFu);
        out[i] = sa + (sb - sa) * (int)t / (int)n;
    }
    return fb_rgb((uint8_t)out[0], (uint8_t)out[1], (uint8_t)out[2]);
}

/* A box with a vertical gradient from top to bottom colour. Corners of radius r
 * are rounded at the top, and at the bottom when round_bottom is set. r = 0 gives
 * a plain box. */
static void fill_box(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t r,
                     uint32_t top, uint32_t bottom, bool round_bottom) {
    if (w == 0 || h == 0) return;
    if (r * 2u > w) r = w / 2u;
    if (r * 2u > h) r = h / 2u;
    for (uint32_t row = 0; row < h; row++) {
        uint32_t inset = 0;
        if (row < r) {
            uint32_t dy = r - row;
            inset = r - isqrt_u32(r * r - dy * dy);
        } else if (round_bottom && row >= h - r) {
            uint32_t dy = r - (h - 1u - row);
            inset = r - isqrt_u32(r * r - dy * dy);
        }
        fb_fill_rect(x + inset, y + row, w - 2u * inset, 1, rgb_mix(top, bottom, row, h - 1u));
    }
}

/* Text with a one-pixel shadow, so it stays readable over the wallpaper. */
static void draw_text_shadow(uint32_t x, uint32_t y, const char *s, uint32_t fg, uint32_t shadow) {
    fb_draw_text(x + 1u, y + 1u, s, shadow, 0, false);
    fb_draw_text(x, y, s, fg, 0, false);
}

/* The round Start orb: a disc shaded from light green at the top to darker at the bottom. */
static void draw_orb(uint32_t cx, uint32_t cy, uint32_t r, bool pressed) {
    uint32_t top = pressed ? 0x2A6E24u : 0x9AE68Au;
    uint32_t bottom = pressed ? 0x163D12u : 0x2C8A2Au;
    for (uint32_t i = 0; i <= 2u * r; i++) {
        int dy = (int)i - (int)r;
        uint32_t half = isqrt_u32(r * r - (uint32_t)(dy * dy));
        fb_fill_rect(cx - half, cy - r + i, 2u * half + 1u, 1, rgb_mix(top, bottom, i, 2u * r));
    }
}

/* How many characters fit in `px` pixels. */
static uint32_t chars_in(uint32_t px) {
    return px / 8u;
}

/* ------------------------------------------------------------------ */
/* Content of the application windows                                  */
/* ------------------------------------------------------------------ */

static void add_line(app_t app, const char *s) {
    if (g_nlines[app] >= MAX_LINES) return;
    put_str(g_text[app][g_nlines[app]++], LINE_LEN, s);
}

/* ------------------------------------------------------------------ */
/* My Computer: an Explorer-style window over the ext4 volume.         */
/* A toolbar (Up and the path), the folder's entries on the left, and  */
/* a preview of the selected file on the right. A click selects an     */
/* entry; a folder opens on a second click or Enter; a file's first    */
/* bytes show in the preview. Backspace goes up one folder. Each folder */
/* is read with ext4_readdir when it is opened, so the list is current. */
/* ------------------------------------------------------------------ */

#define EXP_MAX        64u          /* entries listed for one folder */
#define EXP_DEPTH      12           /* folder levels below the root */
#define EXP_NAME       28           /* longest name kept (longer names are cut) */
#define EXP_PREV_LINES 14           /* lines of a previewed file */
#define EXP_PREV_COLS  30           /* characters per preview line */
#define EXP_PREV_BYTES 512u         /* bytes read for a preview */
#define EXP_TOOL_H     50u          /* toolbar: row 1 the Up button and the path, row 2 the actions */
#define EXP_BTN_Y      28u          /* row 2 buttons: top edge and height */
#define EXP_BTN_H      20u
#define EXP_STAT_H     18u          /* status line at the bottom */
#define EXP_LIST_W     230u         /* the list pane; the preview pane is to its right */

typedef struct {
    uint64_t ino;
    uint64_t size;
    bool dir;
    char name[EXP_NAME];
} exp_entry_t;

static struct {
    uint64_t ino[EXP_DEPTH];          /* the folder at each level; ino[0] is the root */
    char name[EXP_DEPTH][EXP_NAME];   /* that folder's name; name[0] stays empty */
    uint32_t depth;                   /* the folder shown is ino[depth] */
    exp_entry_t ent[EXP_MAX];
    uint32_t n;                       /* entries in use */
    uint32_t sel;                     /* the selected entry */
    uint32_t top;                     /* the first entry drawn in the list */
    bool previewing;                  /* the selected file's first bytes are shown */
    char prev_name[EXP_NAME];
    uint64_t prev_size;
    uint32_t prev_n;
    char prev[EXP_PREV_LINES][EXP_PREV_COLS + 1];
    char status[80];
    bool del_armed;                   /* Delete was pressed once: the next press deletes */
    uint64_t del_ino;                 /* the entry that press was for */
} g_exp;

static bool exp_same(const char *a, const char *b) {
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

/* The folder's path: "/" then the names joined with "/". */
static void exp_path(char *out, size_t cap) {
    size_t at = cat_str(out, cap, 0, "/");
    for (uint32_t d = 1; d <= g_exp.depth; d++) {
        at = cat_str(out, cap, at, g_exp.name[d]);
        if (d < g_exp.depth) at = cat_str(out, cap, at, "/");
    }
}

/* Rows of the list that fit in a client area of height ch. */
static uint32_t exp_rows(uint32_t ch) {
    uint32_t used = EXP_TOOL_H + EXP_STAT_H + 4u;
    return ch > used ? (ch - used) / 12u : 0u;
}

/* Folders sort before files; names compare case-insensitively. */
static int exp_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : (int)c;
}

static bool exp_before(const exp_entry_t *a, const exp_entry_t *b) {
    if (a->dir != b->dir) return a->dir;
    for (uint32_t i = 0; i < EXP_NAME; i++) {
        int ca = exp_lower(a->name[i]);
        int cb = exp_lower(b->name[i]);
        if (ca != cb) return ca < cb;
        if (ca == 0) return false;
    }
    return false;
}

/* Callback for ext4_readdir: keep each entry except "." and "..". */
static bool exp_collect(void *ctx, const ext4_stat_t *st, const char *name, uint32_t name_len) {
    (void)ctx;
    if (name_len == 0) return false;
    if (name[0] == '.' && (name_len == 1 || (name_len == 2 && name[1] == '.'))) return false;
    if (g_exp.n >= EXP_MAX) return true;             /* the list is full */
    exp_entry_t *e = &g_exp.ent[g_exp.n++];
    uint32_t k = name_len < EXP_NAME - 1 ? name_len : EXP_NAME - 1;
    for (uint32_t i = 0; i < k; i++) {
        char c = name[i];
        e->name[i] = (c < 0x20 || c > 0x7e) ? '?' : c;
    }
    e->name[k] = '\0';
    e->ino = st->ino;
    e->size = st->size;
    e->dir = EXT4_S_ISDIR(st->mode);
    return false;
}

static void exp_preview(uint32_t i);

/* Read the folder ino[depth] into the list, sorted, and reset the selection. */
static void exp_load(void) {
    g_exp.n = 0;
    g_exp.sel = 0;
    g_exp.top = 0;
    g_exp.previewing = false;
    g_exp.del_armed = false;
    ext4_fs_t *fs = ext4_get_current();
    if (fs == NULL) {
        put_str(g_exp.status, sizeof g_exp.status, "No ext4 volume is mounted.");
        return;
    }
    ext4_readdir(fs, g_exp.ino[g_exp.depth], exp_collect, NULL);
    /* Insertion sort: the list is short, and it keeps the code small. */
    for (uint32_t i = 1; i < g_exp.n; i++) {
        exp_entry_t tmp = g_exp.ent[i];
        uint32_t j = i;
        while (j > 0 && exp_before(&tmp, &g_exp.ent[j - 1])) {
            g_exp.ent[j] = g_exp.ent[j - 1];
            j--;
        }
        g_exp.ent[j] = tmp;
    }
    size_t at = cat_dec(g_exp.status, sizeof g_exp.status, 0, g_exp.n);
    at = cat_str(g_exp.status, sizeof g_exp.status, at, g_exp.n == 1 ? " item" : " items");
    if (g_exp.n == EXP_MAX) cat_str(g_exp.status, sizeof g_exp.status, at, " (list full)");
    /* The first entry is selected, so a file there is shown at once. */
    if (g_exp.n > 0 && !g_exp.ent[0].dir) exp_preview(0);
}

/* Read the first bytes of file entry i for the preview pane. */
static void exp_preview(uint32_t i) {
    const exp_entry_t *e = &g_exp.ent[i];
    g_exp.previewing = true;
    put_str(g_exp.prev_name, sizeof g_exp.prev_name, e->name);
    g_exp.prev_size = e->size;
    g_exp.prev[0][0] = '\0';
    g_exp.prev_n = 1;
    ext4_fs_t *fs = ext4_get_current();
    if (fs == NULL || e->size == 0) return;
    uint8_t buf[EXP_PREV_BYTES];
    uint64_t got = ext4_read(fs, e->ino, 0, buf, sizeof buf);
    if (got == 0) {
        put_str(g_exp.prev[0], sizeof g_exp.prev[0], "(could not read this file)");
        return;
    }
    /* Newlines start a new line, long lines wrap, and bytes that are not text show as dots. */
    uint32_t line = 0, col = 0;
    for (uint64_t k = 0; k < got; k++) {
        uint8_t c = buf[k];
        if (c == '\r') continue;
        if (c == '\n' || col == EXP_PREV_COLS) {
            if (line + 1 >= EXP_PREV_LINES) break;
            line++;
            col = 0;
            g_exp.prev[line][0] = '\0';
            if (c == '\n') continue;
        }
        g_exp.prev[line][col++] = (c < 0x20 || c > 0x7e) ? '.' : (char)c;
        g_exp.prev[line][col] = '\0';
    }
    g_exp.prev_n = line + 1;
}

static void exp_select(uint32_t i) {
    if (i >= g_exp.n) return;
    g_exp.del_armed = false;
    g_exp.sel = i;
    if (g_exp.ent[i].dir) {
        g_exp.previewing = false;
    } else {
        exp_preview(i);
    }
}

/* Open a folder (it becomes the current one), or preview a file. */
static void exp_open(uint32_t i) {
    if (i >= g_exp.n) return;
    const exp_entry_t *e = &g_exp.ent[i];
    if (!e->dir) {
        exp_preview(i);
        return;
    }
    if (g_exp.depth + 1 >= EXP_DEPTH) {
        put_str(g_exp.status, sizeof g_exp.status, "Folders are nested too deep to open.");
        return;
    }
    uint32_t d = ++g_exp.depth;
    g_exp.ino[d] = e->ino;
    put_str(g_exp.name[d], EXP_NAME, e->name);
    exp_load();
}

/* Up one folder, with the folder we came from selected. */
static void exp_up(void) {
    if (g_exp.depth == 0) return;
    char came[EXP_NAME];
    put_str(came, sizeof came, g_exp.name[g_exp.depth]);
    g_exp.depth--;
    exp_load();
    for (uint32_t i = 0; i < g_exp.n; i++) {
        if (exp_same(g_exp.ent[i].name, came)) {
            g_exp.sel = i;
            break;
        }
    }
}

/* Start the window at the root of the volume. */
static void exp_init(void) {
    g_exp.depth = 0;
    ext4_fs_t *fs = ext4_get_current();
    g_exp.ino[0] = fs != NULL ? ext4_root(fs) : 0;
    g_exp.name[0][0] = '\0';
    exp_load();
}

/* The first name prefix+suffix that is free in dir: "prefix", then "prefix 2suffix", ... */
static bool exp_unique_name(uint64_t dir, const char *prefix, const char *suffix, char *out, size_t cap) {
    ext4_fs_t *fs = ext4_get_current();
    if (fs == NULL) return false;
    for (uint32_t n = 1; n <= 99; n++) {
        size_t at = cat_str(out, cap, 0, prefix);
        if (n > 1) {
            at = cat_str(out, cap, at, " ");
            at = cat_dec(out, cap, at, n);
        }
        cat_str(out, cap, at, suffix);
        ext4_stat_t st;
        if (!ext4_lookup(fs, dir, out, &st)) return true;
    }
    return false;
}

/* Select the entry called name (after a reload), previewing it if it is a file. */
static void exp_select_name(const char *name) {
    for (uint32_t i = 0; i < g_exp.n; i++) {
        if (exp_same(g_exp.ent[i].name, name)) {
            exp_select(i);
            return;
        }
    }
}

/* New folder or New file in the current folder. */
static void exp_new_item(bool folder) {
    ext4_fs_t *fs = ext4_get_current();
    if (fs == NULL) {
        put_str(g_exp.status, sizeof g_exp.status, "No ext4 volume is mounted.");
        return;
    }
    uint64_t dir = g_exp.ino[g_exp.depth];
    char name[EXP_NAME];
    if (!exp_unique_name(dir, folder ? "New folder" : "New file", folder ? "" : ".txt", name, sizeof name)) {
        put_str(g_exp.status, sizeof g_exp.status, "No free name left here.");
        return;
    }
    uint64_t ino = 0;
    bool ok = folder ? ext4_mkdir(fs, dir, name, EXT4_S_IFDIR | 0755, &ino)
                     : ext4_create(fs, dir, name, EXT4_S_IFREG | 0644, &ino);
    if (!ok) {
        put_str(g_exp.status, sizeof g_exp.status, "Could not create it.");
        return;
    }
    exp_load();
    exp_select_name(name);
    cat_str(g_exp.status, sizeof g_exp.status, 0, "Created ");
    cat_str(g_exp.status, sizeof g_exp.status, strlen(g_exp.status), name);
}

/* Copy the selected file into the current folder as "<name> copy". Folders are not copied. */
static void exp_copy_selected(void) {
    ext4_fs_t *fs = ext4_get_current();
    if (fs == NULL || g_exp.n == 0) return;
    const exp_entry_t *e = &g_exp.ent[g_exp.sel];
    if (e->dir) {
        put_str(g_exp.status, sizeof g_exp.status, "Copy works on files, not folders.");
        return;
    }
    uint64_t dir = g_exp.ino[g_exp.depth];
    uint64_t src = e->ino;
    uint64_t size = e->size;
    char prefix[24];
    put_str(prefix, sizeof prefix, e->name);
    cat_str(prefix, sizeof prefix, strlen(prefix), " copy");
    char name[EXP_NAME];
    if (!exp_unique_name(dir, prefix, "", name, sizeof name)) {
        put_str(g_exp.status, sizeof g_exp.status, "No free name left here.");
        return;
    }
    uint64_t dst = 0;
    if (!ext4_create(fs, dir, name, EXT4_S_IFREG | 0644, &dst)) {
        put_str(g_exp.status, sizeof g_exp.status, "Could not create the copy.");
        return;
    }
    uint8_t buf[512];
    for (uint64_t off = 0; off < size;) {
        uint64_t want = size - off < sizeof buf ? size - off : sizeof buf;
        uint64_t got = ext4_read(fs, src, off, buf, want);
        if (got == 0 || ext4_write(fs, dst, off, buf, got) != got) {
            exp_load();
            put_str(g_exp.status, sizeof g_exp.status, "Copy stopped: a read or write failed.");
            return;
        }
        off += got;
    }
    exp_load();
    exp_select_name(name);
    cat_str(g_exp.status, sizeof g_exp.status, 0, "Copied to ");
    cat_str(g_exp.status, sizeof g_exp.status, strlen(g_exp.status), name);
}

/* Delete the selected entry. The first press asks for a second; the second deletes. */
static void exp_delete_selected(void) {
    ext4_fs_t *fs = ext4_get_current();
    if (fs == NULL || g_exp.n == 0) return;
    const exp_entry_t *e = &g_exp.ent[g_exp.sel];
    char name[EXP_NAME];
    put_str(name, sizeof name, e->name);
    if (!g_exp.del_armed || g_exp.del_ino != e->ino) {
        g_exp.del_armed = true;
        g_exp.del_ino = e->ino;
        cat_str(g_exp.status, sizeof g_exp.status, 0, "Press Delete again to delete ");
        cat_str(g_exp.status, sizeof g_exp.status, strlen(g_exp.status), name);
        return;
    }
    g_exp.del_armed = false;
    bool dir = e->dir;
    if (!ext4_unlink(fs, g_exp.ino[g_exp.depth], name, dir)) {
        put_str(g_exp.status, sizeof g_exp.status,
                dir ? "Not deleted: the folder is not empty." : "Not deleted.");
        return;
    }
    exp_load();
    cat_str(g_exp.status, sizeof g_exp.status, 0, "Deleted ");
    cat_str(g_exp.status, sizeof g_exp.status, strlen(g_exp.status), name);
}

static void open_app(app_t app);

/* Open the selected file in the Text Editor. The editor refuses while it holds unsaved text. */
static void exp_edit_selected(void) {
    if (g_exp.n == 0) return;
    const exp_entry_t *e = &g_exp.ent[g_exp.sel];
    if (e->dir) {
        put_str(g_exp.status, sizeof g_exp.status, "Only files open in the editor.");
        return;
    }
    char name[EXP_NAME];
    put_str(name, sizeof name, e->name);
    if (!editor_open_file(&g_ed, g_exp.ino[g_exp.depth], name)) {
        put_str(g_exp.status, sizeof g_exp.status, "Save the open file in the editor first.");
        return;
    }
    open_app(APP_EDITOR);
}

static void build_content(app_t app) {
    if ((unsigned)app >= APP_COUNT) return;     /* the table below is indexed by app */
    g_nlines[app] = 0;
    if (app == APP_ABOUT) {
        add_line(app, "Helix OS desktop (basic test build).");
        add_line(app, "");
        add_line(app, "A Windows 98-style desktop drawn on the");
        add_line(app, "framebuffer by the Helix kernel.");
        add_line(app, "");
        add_line(app, "Tab switches windows, x closes one.");
        add_line(app, "Start > Shut Down, or Esc, leaves the desktop.");
    }
}

/* ------------------------------------------------------------------ */
/* Drawing                                                             */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Terminal window: the kernel shell (the recovery shell). Command output is */
/* captured by a console sink and shown here. `exec <path>` runs a program  */
/* in ring 3 and waits for it, so its output shows up here as well.         */
/* ------------------------------------------------------------------ */

#define TERM_LINES 64
#define TERM_COLS  56               /* characters per line; the window is sized for this */
#define TERM_PROMPT "helix> "
#define TERM_PROMPT_LEN 7

static char g_term_lines[TERM_LINES][TERM_COLS + 1];
static int g_term_n;                /* lines in use; the last one is being written */
static char g_term_in[TERM_COLS + 1];   /* the line being typed */
static int g_term_in_len;
static bool g_term_started;

static void term_newline(void) {
    if (g_term_n == TERM_LINES) {   /* full: scroll everything up one line */
        for (int i = 1; i < TERM_LINES; i++) {
            for (int k = 0; k <= TERM_COLS; k++) g_term_lines[i - 1][k] = g_term_lines[i][k];
        }
    } else {
        g_term_n++;
    }
    g_term_lines[g_term_n - 1][0] = '\0';
}

static void term_putc(char c) {
    if (c == '\r') return;
    if (c == '\n') {
        term_newline();
        return;
    }
    if (c < 0x20) c = ' ';              /* tabs and other control bytes show as a space */
    char *cur = g_term_lines[g_term_n - 1];
    int len = (int)strlen(cur);
    if (len >= TERM_COLS) {             /* wrap long lines */
        term_newline();
        cur = g_term_lines[g_term_n - 1];
        len = 0;
    }
    cur[len] = c;
    cur[len + 1] = '\0';
}

static void term_puts(const char *s) {
    for (; *s != '\0'; s++) term_putc(*s);
}

/* The console sink while a command runs: its output goes into the window. */
static void term_sink(const char *data, size_t len) {
    for (size_t i = 0; i < len; i++) term_putc(data[i]);
}

static void term_welcome(void) {
    term_puts("Helix terminal: kernel shell commands, and exec <path>.\n");
    term_puts("Programs that need the keyboard are not run here:\n");
    term_puts("desktop, usershell, exit, clear, /bin/sh, /bin/init.\n");
}

static void term_key(char c) {
    if (g_term_in_len < TERM_COLS - TERM_PROMPT_LEN) {
        g_term_in[g_term_in_len++] = c;
        g_term_in[g_term_in_len] = '\0';
    }
}

static void term_backspace(void) {
    if (g_term_in_len > 0) {
        g_term_in[--g_term_in_len] = '\0';
    }
}

static bool word_eq(const char *a, const char *b) {
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

/* Copy the next space-separated word from p into out; returns the rest of the line. */
static const char *copy_word(const char *p, char *out, size_t cap) {
    while (*p == ' ') p++;
    size_t n = 0;
    while (*p != '\0' && *p != ' ') {
        if (n + 1 < cap) out[n++] = *p;
        p++;
    }
    out[n] = '\0';
    return p;
}

/* Commands that need the keyboard or would take over the desktop. */
static bool term_blocked(const char *line) {
    char first[16], second[32];
    const char *rest = copy_word(line, first, sizeof first);
    copy_word(rest, second, sizeof second);
    static const char *const blocked[] = { "desktop", "usershell", "exit", "clear" };
    for (size_t i = 0; i < sizeof blocked / sizeof blocked[0]; i++) {
        if (word_eq(first, blocked[i])) return true;
    }
    return word_eq(first, "exec") && (word_eq(second, "/bin/sh") || word_eq(second, "/bin/init"));
}

/* Enter: echo the line, then run it with its output captured into the window. */
static void term_submit(void) {
    char line[TERM_COLS + 1];
    for (int k = 0; k <= g_term_in_len; k++) line[k] = g_term_in[k];
    g_term_in_len = 0;
    g_term_in[0] = '\0';
    term_puts(TERM_PROMPT);
    term_puts(line);
    term_putc('\n');
    if (line[0] == '\0') return;

    klog_event(KLOG_DESKTOP, "terminal: %s", line);
    if (term_blocked(line)) {
        term_puts("not run here: it needs the keyboard or is the desktop\n");
        return;
    }
    char work[TERM_COLS + 1];           /* the shell edits its line in place */
    for (int k = 0; k <= g_term_in_len + (int)strlen(line); k++) work[k] = line[k];
    console_sink_fn previous = console_get_sink();
    console_set_sink(term_sink);
    shell_run_line(work);
    console_set_sink(previous);
}

/* The text area: the newest lines, bottom-aligned, with the line being typed last. */
static void term_draw(uint32_t cx, uint32_t cy, uint32_t cw, uint32_t ch) {
    fb_fill_rect(cx, cy, cw, ch, c_black);
    int rows = (int)((ch - 8u) / 12u);
    int total = g_term_n + 1;           /* the buffer lines, then the input line */
    int first = total > rows ? total - rows : 0;
    for (int r = 0; r < rows && first + r < total; r++) {
        int idx = first + r;
        uint32_t y = cy + 4u + (uint32_t)r * 12u;
        if (idx < g_term_n) {
            fb_draw_text(cx + 4, y, g_term_lines[idx], c_white, 0, false);
        } else {
            char row[TERM_COLS + 2];
            int k = 0;
            for (const char *p = TERM_PROMPT; *p != '\0'; p++) row[k++] = *p;
            for (int i = 0; i < g_term_in_len; i++) row[k++] = g_term_in[i];
            row[k++] = '_';
            row[k] = '\0';
            fb_draw_text(cx + 4, y, row, c_white, 0, false);
        }
    }
}

/* Text Editor layout inside its window: the text area is above a strip that
 * holds the status text and the Save button. */
#define ED_STRIP_H 22u
#define ED_SAVE_W  56u

static uint32_t ed_rows(uint32_t ch) {
    return (ch - ED_STRIP_H - 4u) / 12u;      /* 12 px per line, 4 px top margin */
}

#define ED_MAX_ROWS 32u
#define ED_MAX_COLS 96u

/* What the editor's client area currently shows, kept so a keystroke can repaint just
 * the rows that changed instead of the whole window. Valid only after draw_editor ran. */
static struct {
    bool valid;
    uint32_t cx, cy, cw, ch;                  /* the client area it was drawn into */
    char rows[ED_MAX_ROWS][ED_MAX_COLS + 1];  /* the text drawn on each row */
    int caret_row;                            /* -1: no caret shown */
    uint32_t caret_off;                       /* caret column within that row */
    char msg[sizeof g_ed.msg];                /* the status text in the strip */
} g_ed_shown;

static uint32_t ed_row_count(uint32_t ch) {
    uint32_t rows = ed_rows(ch);
    return rows > ED_MAX_ROWS ? ED_MAX_ROWS : rows;
}

static uint32_t ed_col_count(uint32_t cw) {
    uint32_t cols = (cw - 8u) / 8u;
    return cols > ED_MAX_COLS ? ED_MAX_COLS : cols;
}

/* Work out the text of each visible row, and which row and column hold the caret. */
static void ed_layout(uint32_t rows, uint32_t cols, char (*out)[ED_MAX_COLS + 1],
                      int *caret_row, uint32_t *caret_off) {
    uint32_t cur_line = editor_line_of(&g_ed, g_ed.pos);
    *caret_row = -1;
    *caret_off = 0;
    for (uint32_t r = 0; r < rows; r++) out[r][0] = '\0';

    uint32_t off = editor_offset_of_line(&g_ed, g_ed.top);
    for (uint32_t r = 0; r < rows && off <= g_ed.len; r++) {
        uint32_t line = g_ed.top + r;
        uint32_t end = editor_line_end(&g_ed, off);
        uint32_t n = 0;
        for (uint32_t i = off; i < end && n < cols; i++) out[r][n++] = g_ed.text[i];
        out[r][n] = '\0';
        if (line == cur_line) {
            *caret_row = (int)r;
            *caret_off = g_ed.pos - off;
        }
        if (end >= g_ed.len) break;
        off = end + 1;
    }
}

/* Draw one row's text and, if it holds the caret, the caret. The background is the caller's. */
static void ed_paint_row(uint32_t cx, uint32_t cy, uint32_t cw, uint32_t r, const char *text,
                         bool has_caret, uint32_t caret_off) {
    uint32_t y = cy + 4u + r * 12u;
    fb_draw_text(cx + 4, y, text, c_ink, 0, false);
    if (has_caret) {
        uint32_t px = cx + 4u + caret_off * 8u;
        if (px < cx + cw - 4u) fb_fill_rect(px, y, 1, 10, c_ink);   /* the caret */
    }
}

/* The strip under the text: file name, status text, and the Save button. */
static void ed_draw_strip(uint32_t cx, uint32_t cy, uint32_t cw, uint32_t ch) {
    uint32_t sy = cy + ch - ED_STRIP_H;
    fb_fill_rect(cx, sy, cw, ED_STRIP_H, c_face);
    fb_draw_text(cx + 4, sy + 7, editor_title(&g_ed), c_ink, 0, false);
    fb_draw_text(cx + 4 + (uint32_t)strlen(editor_title(&g_ed)) * 8u + 16u, sy + 7, g_ed.msg, c_ink, 0, false);
    uint32_t bx = cx + cw - ED_SAVE_W - 4u, by = sy + 3u;
    fb_fill_rect(bx, by, ED_SAVE_W, 16, c_face);
    bevel(bx, by, ED_SAVE_W, 16, true);
    fb_draw_text(bx + 12, by + 4, "Save", c_ink, 0, false);
}

/* Paint the editor's client area: the visible lines, the caret, the status strip. */
static void draw_editor(uint32_t cx, uint32_t cy, uint32_t cw, uint32_t ch) {
    uint32_t rows = ed_row_count(ch);
    editor_scroll_to_cursor(&g_ed, rows);
    uint32_t cols = ed_col_count(cw);
    fb_fill_rect(cx, cy, cw, ch - ED_STRIP_H, c_client);

    uint32_t caret_off;
    ed_layout(rows, cols, g_ed_shown.rows, &g_ed_shown.caret_row, &caret_off);
    for (uint32_t r = 0; r < rows; r++) {
        ed_paint_row(cx, cy, cw, r, g_ed_shown.rows[r], (int)r == g_ed_shown.caret_row, caret_off);
    }
    ed_draw_strip(cx, cy, cw, ch);

    g_ed_shown.caret_off = caret_off;
    g_ed_shown.cx = cx;
    g_ed_shown.cy = cy;
    g_ed_shown.cw = cw;
    g_ed_shown.ch = ch;
    memcpy(g_ed_shown.msg, g_ed.msg, sizeof g_ed_shown.msg);
    g_ed_shown.valid = true;
}

/* ---- Browser window ----
 * An address bar, a status line, and the page as plain text. Enter fetches the address
 * with net_http_get and shows the body as text (html_to_text strips the tags). The fetch
 * blocks the desktop until the response arrives or times out. Only plain http:// works. */
#define BR_URL_CAP   240
#define BR_RAW_CAP   (256u * 1024u)   /* the response, then its body in place */
#define BR_TEXT_CAP  (64u * 1024u)    /* the page as plain text */
#define BR_COLS      52               /* characters per page row (leaves room for the scrollbar) */
#define BR_BAR_W     14u              /* scrollbar width, drawn at the right of the page */
#define BR_ARROW     14u              /* height of each scrollbar arrow button */
#define BR_VISIBLE   21               /* page rows shown: the window is 340 px high, see open_app */
#define BR_BAR_H     18u
#define BR_STATUS_H  20u
#define BR_MAX_HOPS  3

typedef struct {
    char url[BR_URL_CAP];
    uint32_t url_len;
    char status[120];
    uint32_t text_len;
    uint32_t top;               /* first page row shown */
} browser_t;

static browser_t g_br = { .status = "Type an address and press Enter." };
static uint8_t g_br_raw[BR_RAW_CAP];
static char g_br_text[BR_TEXT_CAP];

/* Append s, then the decimal value, to a bounded string. */
static void br_append(char *d, uint32_t cap, const char *s) {
    uint32_t n = (uint32_t)strlen(d);
    while (*s != '\0' && n + 1 < cap) d[n++] = *s++;
    d[n] = '\0';
}

static void br_append_uint(char *d, uint32_t cap, uint32_t v) {
    char digits[12];
    uint32_t k = 0;
    do {
        digits[k++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v != 0);
    char text[12];
    uint32_t m = 0;
    while (k > 0) text[m++] = digits[--k];
    text[m] = '\0';
    br_append(d, cap, text);
}

/* Lay the page text out in rows of at most BR_COLS characters, breaking at newlines, and
 * paint rows g_br.top .. top+visible-1 when paint is set. Returns the total row count. */
static uint32_t br_layout(bool paint, uint32_t px, uint32_t py, uint32_t visible) {
    uint32_t r = 0;
    uint32_t n = g_br.text_len;
    uint32_t start = 0;
    for (;;) {
        uint32_t end = start;
        while (end < n && g_br_text[end] != '\n') end++;
        uint32_t len = end - start;
        uint32_t k = 0;
        do {                                   /* an empty line still takes a row */
            uint32_t rest = len - k;
            uint32_t take = rest, adv = rest;
            if (rest > BR_COLS) {
                /* Break at the last space that fits; a word longer than a row is cut. */
                take = BR_COLS;
                adv = BR_COLS;
                for (uint32_t b = BR_COLS; b > 0; b--) {
                    if (g_br_text[start + k + b] == ' ') {
                        take = b;
                        adv = b + 1;               /* skip the space itself */
                        break;
                    }
                }
            }
            if (paint && r >= g_br.top && r < g_br.top + visible) {
                char row[BR_COLS + 1];
                for (uint32_t j = 0; j < take; j++) row[j] = g_br_text[start + k + j];
                row[take] = '\0';
                fb_draw_text(px, py + (r - g_br.top) * 12u, row, c_ink, 0, false);
            }
            r++;
            k += adv;
        } while (k < len);
        if (end >= n) break;
        start = end + 1;
    }
    return r;
}

/* Scrollbar geometry for a page of rows rows, whose bar starts at by: the track sits
 * between the two arrow buttons and the thumb's size and place show how much of the
 * page is on screen and where it is. */
static void br_bar_geometry(uint32_t by, uint32_t rows, uint32_t *track_y, uint32_t *track_h,
                            uint32_t *thumb_y, uint32_t *thumb_h) {
    *track_y = by + BR_ARROW;
    *track_h = BR_VISIBLE * 12u - 2u * BR_ARROW;
    uint32_t th = rows > BR_VISIBLE ? (*track_h * BR_VISIBLE) / rows : *track_h;
    if (th < 14u) th = 14u;                  /* always grabbable */
    if (th > *track_h) th = *track_h;
    *thumb_h = th;
    uint32_t max_top = rows > BR_VISIBLE ? rows - BR_VISIBLE : 0;
    uint32_t room = *track_h - th;
    *thumb_y = *track_y + (max_top > 0 ? (room * g_br.top) / max_top : 0u);
}

static void browser_scrollbar(uint32_t bx, uint32_t by, uint32_t rows) {
    uint32_t bar_h = BR_VISIBLE * 12u;
    fb_fill_rect(bx, by, BR_BAR_W, bar_h, rgb_hex(g_th->track));
    bevel(bx, by, BR_BAR_W, bar_h, false);
    fb_draw_text(bx + 3u, by + 3u, "^", c_ink, 0, false);
    fb_draw_text(bx + 3u, by + bar_h - BR_ARROW + 3u, "v", c_ink, 0, false);
    uint32_t track_y, track_h, thumb_y, thumb_h;
    br_bar_geometry(by, rows, &track_y, &track_h, &thumb_y, &thumb_h);
    fill_box(bx + 2u, thumb_y, BR_BAR_W - 4u, thumb_h, 3, g_th->thumb_top, g_th->thumb_bot, true);
}

static void browser_draw(uint32_t cx, uint32_t cy, uint32_t cw, uint32_t ch) {
    fb_fill_rect(cx, cy, cw, ch, c_client);

    /* Address bar: a sunken box holding the end of the address, with a caret. */
    uint32_t bx = cx + 4u, by = cy + 4u, bw = cw - 8u;
    fb_fill_rect(bx, by, bw, BR_BAR_H, c_client);
    bevel(bx, by, bw, BR_BAR_H, false);
    uint32_t fits = (bw - 8u) / 8u;
    uint32_t from = g_br.url_len > fits ? g_br.url_len - fits : 0;
    char shown[BR_URL_CAP];
    uint32_t k = 0;
    for (uint32_t i = from; i < g_br.url_len; i++) shown[k++] = g_br.url[i];
    shown[k] = '\0';
    fb_draw_text(bx + 4u, by + 5u, shown, c_ink, 0, false);
    fb_fill_rect(bx + 4u + (g_br.url_len - from) * 8u, by + 3u, 1, 12, c_ink);   /* caret */

    /* Page: plain text rows below the address bar. */
    uint32_t py = cy + BR_BAR_H + 12u;
    uint32_t rows = br_layout(true, cx + 6u, py, BR_VISIBLE);
    browser_scrollbar(cx + cw - 6u - BR_BAR_W, py, rows);

    /* Status line at the bottom. */
    uint32_t sy = cy + ch - BR_STATUS_H;
    fb_fill_rect(cx, sy, cw, BR_STATUS_H, c_face);
    uint32_t scols = (cw - 8u) / 8u;
    char st[120];
    uint32_t sn = 0;
    while (g_br.status[sn] != '\0' && sn < scols && sn + 1 < sizeof st) {
        st[sn] = g_br.status[sn];
        sn++;
    }
    st[sn] = '\0';
    fb_draw_text(cx + 4u, sy + 6u, st, c_ink, 0, false);
}

/* A raised button with its label centred; the label is grey when the button is off. */
static void win_button(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const char *label, bool enabled) {
    fb_fill_rect(x, y, w, h, c_face);
    bevel(x, y, w, h, true);
    uint32_t tw = (uint32_t)strlen(label) * 8u;
    uint32_t tx = x + (w > tw ? (w - tw) / 2u : 2u);
    uint32_t ty = y + (h > 8u ? (h - 8u) / 2u : 0u);
    fb_draw_text(tx, ty, label, enabled ? c_ink : c_gray, 0, false);
}

/* Draw a line of text clipped to cols characters and to the bottom edge. */
static void exp_text(uint32_t x, uint32_t y, uint32_t bottom, uint32_t cols, const char *s, uint32_t color) {
    if (y + 10u > bottom) return;
    char t[80];
    put_str(t, sizeof t, s);
    if (cols < sizeof t && strlen(t) > cols) t[cols] = '\0';
    fb_draw_text(x, y, t, color, 0, false);
}

/* ------------------------------------------------------------------ */
/* Network window: ping and name lookup over the kernel's network stack. */
/* Enter pings the host. A name is looked up first; a dotted address is */
/* used as it is. Each call waits for the network, so the desktop waits too. */
/* ------------------------------------------------------------------ */

#define NET_HOST_CAP 40
#define NET_LINES    12
#define NET_COLS     52

static struct {
    char host[NET_HOST_CAP];
    uint32_t host_len;
    char out[NET_LINES][NET_COLS];
    uint32_t out_n;
} g_net;

/* Add a line to the log; when it is full, the oldest line goes. */
static void net_out(const char *s) {
    if (g_net.out_n == NET_LINES) {
        for (uint32_t i = 1; i < NET_LINES; i++) {
            for (uint32_t k = 0; k < NET_COLS; k++) g_net.out[i - 1][k] = g_net.out[i][k];
        }
        g_net.out_n = NET_LINES - 1;
    }
    put_str(g_net.out[g_net.out_n++], NET_COLS, s);
}

/* "a.b.c.d" for an address, into out. */
static void net_fmt_ip(char *out, size_t cap, const uint8_t ip[4]) {
    size_t at = 0;
    out[0] = '\0';
    for (int i = 0; i < 4; i++) {
        at = cat_dec(out, cap, at, ip[i]);
        if (i < 3) at = cat_str(out, cap, at, ".");
    }
}

/* Resolve the host (unless it is an address), then ping it four times if asked. */
static void net_run(bool do_ping) {
    if (g_net.host_len == 0) {
        net_out("Type a host name or address first.");
        return;
    }
    g_net.host[g_net.host_len] = '\0';
    uint8_t ip[4];
    char line[NET_COLS];
    if (net_parse_ip(g_net.host, ip)) {
        net_fmt_ip(line, sizeof line, ip);
    } else {
        if (net_dns_resolve(g_net.host, ip, 3000) != NET_DNS_OK) {
            net_out("Lookup failed: no answer for that name.");
            return;
        }
        size_t at = cat_str(line, sizeof line, 0, g_net.host);
        at = cat_str(line, sizeof line, at, " is at ");
        char addr[24];
        net_fmt_ip(addr, sizeof addr, ip);
        cat_str(line, sizeof line, at, addr);
    }
    net_out(line);
    if (!do_ping) return;
    for (uint16_t seq = 1; seq <= 4; seq++) {
        uint32_t rtt_us = 0;
        char addr[24];
        net_fmt_ip(addr, sizeof addr, ip);
        if (net_ping(ip, seq, 1000, &rtt_us)) {
            size_t at = cat_str(line, sizeof line, 0, "Reply from ");
            at = cat_str(line, sizeof line, at, addr);
            at = cat_str(line, sizeof line, at, ": time ");
            if (rtt_us < 1000u) {
                cat_str(line, sizeof line, at, "under 1 ms");
            } else {
                at = cat_dec(line, sizeof line, at, rtt_us / 1000u);
                cat_str(line, sizeof line, at, " ms");
            }
        } else {
            size_t at = cat_str(line, sizeof line, 0, "No reply from ");
            cat_str(line, sizeof line, at, addr);
        }
        net_out(line);
    }
}

static void network_draw(uint32_t cx, uint32_t cy, uint32_t cw, uint32_t ch) {
    fb_fill_rect(cx, cy, cw, ch, c_client);

    /* The host field: a sunken box with the end of the name and a caret. */
    fb_draw_text(cx + 8u, cy + 11u, "Host", c_ink, 0, false);
    uint32_t fx = cx + 52u, fy = cy + 6u, fw = cw - 60u;
    fb_fill_rect(fx, fy, fw, 20, c_client);
    bevel(fx, fy, fw, 20, false);
    uint32_t fits = (fw - 8u) / 8u;
    uint32_t from = g_net.host_len > fits ? g_net.host_len - fits : 0;
    char shown[NET_HOST_CAP];
    uint32_t k = 0;
    for (uint32_t i = from; i < g_net.host_len; i++) shown[k++] = g_net.host[i];
    shown[k] = '\0';
    fb_draw_text(fx + 4u, fy + 6u, shown, c_ink, 0, false);
    fb_fill_rect(fx + 4u + k * 8u, fy + 4u, 1, 12, c_ink);      /* caret */

    win_button(cx + 52u, cy + 34u, 64u, 22u, "Ping", true);
    win_button(cx + 124u, cy + 34u, 84u, 22u, "Lookup", true);

    /* The log, newest last; the status bar says what Enter does. */
    uint32_t bottom = cy + ch - 18u;
    for (uint32_t i = 0; i < g_net.out_n; i++) {
        uint32_t y = cy + 66u + i * 12u;
        if (y + 10u > bottom) break;
        exp_text(cx + 8u, y, bottom, (cw - 16u) / 8u, g_net.out[i], c_ink);
    }
    uint32_t sy = cy + ch - 18u;
    fb_fill_rect(cx, sy, cw, 18, c_face);
    exp_text(cx + 4u, sy + 5u, cy + ch, (cw - 8u) / 8u, "Enter pings. The desktop waits for the network.", c_ink);
}

/* My Computer's client area: toolbar, folder list, preview pane, status line. */
static void exp_draw(uint32_t cx, uint32_t cy, uint32_t cw, uint32_t ch) {
    fb_fill_rect(cx, cy, cw, ch, c_client);

    /* Toolbar: the Up button (greyed at the root) and the path. */
    fb_fill_rect(cx, cy, cw, EXP_TOOL_H, c_face);
    fb_fill_rect(cx, cy + EXP_TOOL_H - 1u, cw, 1, c_dark);
    fb_fill_rect(cx + 4u, cy + 3u, 40, 20, c_face);
    bevel(cx + 4u, cy + 3u, 40, 20, true);
    fb_draw_text(cx + 12u, cy + 9u, "Up", g_exp.depth > 0 ? c_ink : c_gray, 0, false);
    char path[160];
    exp_path(path, sizeof path);
    uint32_t pcols = (cw - 56u) / 8u;
    if (pcols >= sizeof path) pcols = sizeof path - 1u;
    if (strlen(path) > pcols) path[pcols] = '\0';
    fb_draw_text(cx + 52u, cy + 9u, path, c_ink, 0, false);
    bool file_sel = g_exp.n > 0 && !g_exp.ent[g_exp.sel].dir;
    uint32_t by = cy + EXP_BTN_Y;
    win_button(cx + 4u, by, 84u, EXP_BTN_H, "New folder", true);
    win_button(cx + 92u, by, 76u, EXP_BTN_H, "New file", true);
    win_button(cx + 172u, by, 56u, EXP_BTN_H, "Copy", file_sel);
    win_button(cx + 232u, by, 56u, EXP_BTN_H, "Edit", file_sel);
    win_button(cx + 292u, by, 64u, EXP_BTN_H, "Delete", g_exp.n > 0);

    /* The list pane: keep the selected entry on screen, then draw the visible rows. */
    uint32_t rows = exp_rows(ch);
    if (g_exp.sel < g_exp.top) g_exp.top = g_exp.sel;
    if (rows > 0 && g_exp.sel >= g_exp.top + rows) g_exp.top = g_exp.sel + 1u - rows;

    uint32_t body_y = cy + EXP_TOOL_H;
    uint32_t body_h = ch - EXP_TOOL_H - EXP_STAT_H;
    uint32_t list_y = body_y + 2u;
    fb_fill_rect(cx + EXP_LIST_W, body_y, 1, body_h, c_dark);      /* divider */
    if (g_exp.n == 0) {
        fb_draw_text(cx + 6u, list_y, "This folder is empty.", c_ink, 0, false);
    }
    for (uint32_t r = 0; r < rows; r++) {
        uint32_t i = g_exp.top + r;
        if (i >= g_exp.n) break;
        const exp_entry_t *e = &g_exp.ent[i];
        uint32_t y = list_y + r * 12u;
        bool sel = (i == g_exp.sel);
        if (sel) fb_fill_rect(cx + 2u, y, EXP_LIST_W - 4u, 12, fb_rgb(0x2F, 0x6E, 0xB8));
        char label[19];
        put_str(label, sizeof label, e->name);
        fb_draw_text(cx + 6u, y + 2u, label, sel ? c_white : c_ink, 0, false);
        char info[12];
        if (e->dir) {
            put_str(info, sizeof info, "<DIR>");
        } else {
            size_t at = cat_dec(info, sizeof info, 0, e->size);
            cat_str(info, sizeof info, at, " B");
        }
        fb_draw_text(cx + EXP_LIST_W - 72u, y + 2u, info, sel ? c_white : c_ink, 0, false);
    }

    /* The preview pane: a file's name, size and first lines, or a folder's hint. */
    uint32_t px = cx + EXP_LIST_W + 8u;
    uint32_t prev_cols = (cw - (EXP_LIST_W + 8u) - 6u) / 8u;
    uint32_t pbottom = cy + ch - EXP_STAT_H;
    if (g_exp.n > 0 && g_exp.previewing) {
        exp_text(px, list_y, pbottom, prev_cols, g_exp.prev_name, c_navy);
        char info[48];
        size_t at = cat_dec(info, sizeof info, 0, g_exp.prev_size);
        at = cat_str(info, sizeof info, at, " bytes");
        if (g_exp.prev_size > EXP_PREV_BYTES) cat_str(info, sizeof info, at, ", first 512 shown");
        exp_text(px, list_y + 14u, pbottom, prev_cols, info, c_ink);
        uint32_t ty = list_y + 32u;
        if (g_exp.prev_size == 0) {
            exp_text(px, ty, pbottom, prev_cols, "(empty file)", c_ink);
        } else {
            for (uint32_t r = 0; r < g_exp.prev_n; r++) {
                exp_text(px, ty + r * 12u, pbottom, prev_cols, g_exp.prev[r], c_ink);
            }
        }
    } else if (g_exp.n > 0 && g_exp.ent[g_exp.sel].dir) {
        exp_text(px, list_y, pbottom, prev_cols, g_exp.ent[g_exp.sel].name, c_navy);
        exp_text(px, list_y + 16u, pbottom, prev_cols, "Folder. Press Enter, or click", c_ink);
        exp_text(px, list_y + 28u, pbottom, prev_cols, "it again, to open it.", c_ink);
    }

    /* Status line. */
    uint32_t sy = cy + ch - EXP_STAT_H;
    fb_fill_rect(cx, sy, cw, EXP_STAT_H, c_face);
    exp_text(cx + 4u, sy + 4u, cy + ch, (cw - 8u) / 8u, g_exp.status, c_ink);
}

/* One line of the Settings page; advances y to the next line. */
#define SET_THEME_W 240u
#define SET_THEME_H 22u

/* The Appearance button: a raised button whose label names the theme it switches to. */
static void settings_theme_button(uint32_t bx, uint32_t by) {
    fb_fill_rect(bx, by, SET_THEME_W, SET_THEME_H, c_face);
    bevel(bx, by, SET_THEME_W, SET_THEME_H, true);
    char label[40];
    put_str(label, sizeof(label), "Theme: ");
    size_t n = 0;
    while (label[n] != '\0') n++;
    const char *name = g_theme_names[g_theme_idx];
    for (size_t i = 0; name[i] != '\0' && n + 1 < sizeof(label); i++) label[n++] = name[i];
    label[n] = '\0';
    fb_draw_text(bx + 8u, by + 7u, label, c_ink, 0, false);
}

static void settings_line(uint32_t x, uint32_t *y, uint32_t color, const char *s) {
    fb_draw_text(x, *y, s, color, 0, false);
    *y += 14u;
}

/* The Settings page: read-only system information. Nothing here can be changed yet. */
static void settings_draw(uint32_t cx, uint32_t cy, uint32_t cw, uint32_t ch) {
    (void)cw;
    (void)ch;
    fb_fill_rect(cx, cy, cw, ch, c_client);
    uint32_t x = cx + 10u;
    uint32_t y = cy + 8u;
    char line[80];

    settings_line(x, &y, c_navy, "Appearance");
    settings_theme_button(x, y + 2u);
    y += 30u;

    settings_line(x, &y, c_navy, "Account");
    const char *who = accounts_session();
    put_str(line, sizeof line, "Signed in as: ");
    cat_str(line, sizeof line, strlen(line), (who != NULL && who[0] != '\0') ? who : "(nobody)");
    settings_line(x, &y, c_ink, line);
    put_str(line, sizeof line, "Accounts on this system: ");
    cat_dec(line, sizeof line, strlen(line), accounts_count());
    settings_line(x, &y, c_ink, line);
    settings_line(x, &y, c_ink, accounts_persistent() ? "Saved to /etc/users."
                                                       : "Kept in memory only, not saved.");
    y += 8u;

    settings_line(x, &y, c_navy, "Display");
    put_str(line, sizeof line, "Screen: ");
    size_t at = cat_dec(line, sizeof line, strlen(line), fb_width());
    at = cat_str(line, sizeof line, at, " x ");
    cat_dec(line, sizeof line, at, fb_height());
    settings_line(x, &y, c_ink, line);
    settings_line(x, &y, c_ink, "Wallpaper: sky blue (the only choice).");
    y += 8u;

    settings_line(x, &y, c_navy, "System");
    put_str(line, sizeof line, "Version: ");
    cat_str(line, sizeof line, strlen(line), HELIX_VERSION);
    settings_line(x, &y, c_ink, line);
    settings_line(x, &y, c_ink, "Clock: UTC.");
    y += 8u;
    settings_line(x, &y, c_gray, "The theme: press t, or use the button.");
}

static void draw_window(const window_t *w, bool focused) {
    /* Shadow: two dark offset boxes under the window. They are solid, not blurred, because the
     * framebuffer has no backbuffer to blur from. A maximised window casts none. */
    if (!w->maximised) {
        fill_box(w->x + 1, w->y + 2, w->w + 2, w->h + 2, 6, 0x1E2B3Cu, 0x1E2B3Cu, false);
        fill_box(w->x + 3, w->y + 5, w->w + 2, w->h + 2, 6, 0x0B1424u, 0x0B1424u, false);
    }
    /* Glass frame with a darker outline. */
    fill_box(w->x, w->y, w->w, w->h, 6, g_th->frame_rim, g_th->frame_rim, false);
    if (focused) {
        fill_box(w->x + 1, w->y + 1, w->w - 2, w->h - 2, 5, g_th->frame_focus_top, g_th->frame_focus_bot, false);
    } else {
        fill_box(w->x + 1, w->y + 1, w->w - 2, w->h - 2, 5, g_th->frame_blur_top, g_th->frame_blur_bot, false);
    }

    if (!w->maximised) {                       /* resize grip: three dots in the corner */
        for (uint32_t k = 0; k < 3; k++) fb_fill_rect(w->x + w->w - 4u - k * 4u, w->y + w->h - 4u, 2, 2, c_gray);
    }

    /* Caption: the title on the glass, then minimise, maximise and a red close button. */
    uint32_t tx = w->x + 3, ty = w->y + 3, tw = w->w - 6;
    uint32_t cap = focused ? rgb_hex(g_th->cap_focus) : rgb_hex(g_th->cap_blur);
    fb_draw_text(tx + 8, ty + 5, g_app_names[w->app], cap, 0, false);

    uint32_t glyph = rgb_hex(g_th->tbtn_focus_text);
    uint32_t mnx = w->x + w->w - 57u, mxx = w->x + w->w - 39u;
    fill_box(mnx, ty + 2, 16, 14, 3, g_th->tbtn_focus_top, g_th->tbtn_focus_bot, true);
    fb_fill_rect(mnx + 4u, ty + 11u, 8, 2, glyph);                       /* _ */
    fill_box(mxx, ty + 2, 16, 14, 3, g_th->tbtn_focus_top, g_th->tbtn_focus_bot, true);
    fb_fill_rect(mxx + 4u, ty + 5u, 8, 1, glyph);                        /* [] */
    fb_fill_rect(mxx + 4u, ty + 9u, 8, 1, glyph);
    fb_fill_rect(mxx + 4u, ty + 5u, 1, 5, glyph);
    fb_fill_rect(mxx + 11u, ty + 5u, 1, 5, glyph);

    uint32_t bx = tx + tw - 18;
    fill_box(bx, ty + 2, 16, 14, 3, 0xE8A38Cu, 0xC2472Cu, true);
    fb_draw_text(bx + 4, ty + 5, "x", fb_rgb(0xFF, 0xFF, 0xFF), 0, false);

    /* Client area: white with a thin blue-grey edge. */
    uint32_t cx = w->x + 6, cy = w->y + 26;
    uint32_t cw = w->w - 12, ch = w->h - 32;
    uint32_t edge = rgb_hex(g_th->client_edge);
    fb_fill_rect(cx, cy, cw, ch, c_client);
    fb_fill_rect(cx, cy, cw, 1, edge);
    fb_fill_rect(cx, cy + ch - 1, cw, 1, edge);
    fb_fill_rect(cx, cy, 1, ch, edge);
    fb_fill_rect(cx + cw - 1, cy, 1, ch, edge);
    if (w->app == APP_TERMINAL) {
        term_draw(cx, cy, cw, ch);
        return;
    }
    if (w->app == APP_EDITOR) {
        draw_editor(cx, cy, cw, ch);
        return;
    }
    if (w->app == APP_BROWSER) {
        browser_draw(cx, cy, cw, ch);
        return;
    }
    if (w->app == APP_MYCOMP) {
        exp_draw(cx, cy, cw, ch);
        return;
    }
    if (w->app == APP_SETTINGS) {
        settings_draw(cx, cy, cw, ch);
        return;
    }
    if (w->app == APP_NETWORK) {
        network_draw(cx, cy, cw, ch);
        return;
    }
    uint32_t cols = chars_in(cw - 8);
    for (int i = 0; i < g_nlines[w->app]; i++) {
        uint32_t row_y = cy + 6 + (uint32_t)i * 12u;
        if (row_y + 8 > cy + ch) break;
        char line[LINE_LEN];
        put_str(line, sizeof(line), g_text[w->app][i]);
        if (cols < sizeof(line)) line[cols] = '\0';
        fb_draw_text(cx + 4, row_y, line, c_ink, 0, false);
    }
}

/* Oxygen icon name for each desktop icon, from the icon pack (see icon_pack.h). */
static const char *const g_icon_name[ICON_COUNT] = {
    "computer", "help-about", "utilities-terminal", "accessories-text-editor", "internet-web-browser",
};

static void draw_icon(int idx, uint32_t x, uint32_t y, bool selected) {
    /* A 32x32 picture per icon. The Oxygen picture is used when the pack has it; the
     * drawn fallbacks below (a monitor, a page, a terminal) keep the desktop usable without it. */
    const uint8_t *picture = icon_pack_find(g_icon_name[idx]);
    if (picture != NULL) {
        fb_blit_bgra(x, y, 32, 32, picture);
    } else if (idx == 0) {
        fb_fill_rect(x + 4, y + 2, 24, 18, c_gray);
        fb_fill_rect(x + 7, y + 5, 18, 12, c_navy);
        fb_fill_rect(x + 12, y + 22, 8, 4, c_gray);
        fb_fill_rect(x + 8, y + 26, 16, 3, c_gray);
    } else if (idx == 1) {
        fb_fill_rect(x + 8, y + 2, 18, 26, c_white);
        bevel(x + 8, y + 2, 18, 26, false);
        fb_fill_rect(x + 13, y + 10, 8, 2, c_navy);
        fb_fill_rect(x + 13, y + 15, 8, 2, c_navy);
        fb_fill_rect(x + 13, y + 20, 6, 2, c_navy);
    } else if (idx == 2) {
        fb_fill_rect(x + 4, y + 2, 24, 20, c_gray);
        fb_fill_rect(x + 7, y + 5, 18, 14, c_black);
        fb_draw_text(x + 8, y + 8, ">_", c_white, 0, false);
        fb_fill_rect(x + 10, y + 24, 12, 3, c_gray);
        fb_fill_rect(x + 7, y + 27, 18, 3, c_gray);
    } else if (idx == 4) {
        /* a window with an address bar and a page below it */
        fb_fill_rect(x + 4, y + 2, 24, 26, c_gray);
        fb_fill_rect(x + 6, y + 4, 20, 4, c_navy);
        fb_fill_rect(x + 6, y + 10, 20, 16, c_white);
        bevel(x + 6, y + 10, 20, 16, false);
        fb_fill_rect(x + 9, y + 14, 14, 2, c_navy);
        fb_fill_rect(x + 9, y + 19, 10, 2, c_navy);
    } else {
        /* a page with a few lines of text */
        fb_fill_rect(x + 8, y + 2, 18, 26, c_white);
        bevel(x + 8, y + 2, 18, 26, false);
        fb_fill_rect(x + 11, y + 8, 12, 2, c_navy);
        fb_fill_rect(x + 11, y + 14, 12, 2, c_navy);
        fb_fill_rect(x + 11, y + 20, 8, 2, c_navy);
    }
    const char *name = g_app_names[g_icon_app[idx]];
    uint32_t label_x = x + 4;
    uint32_t label_w = (uint32_t)strlen(name) * 8u + 4u;
    if (selected) {
        fill_box(label_x - 2, y + 36, label_w, 14, 3, 0x4A8AD2u, 0x2A5FA3u, true);
    }
    draw_text_shadow(label_x, y + 38, name, fb_rgb(0xFF, 0xFF, 0xFF), fb_rgb(0x0B, 0x1F, 0x3A));
}

/* Start menu rows are built the same way for drawing and for clicks. A row is a section
 * title, a rule, or a choice that runs a program. */
enum { MROW_ITEM, MROW_HEADER, MROW_SEP };
typedef struct {
    int kind;
    app_t app;
    const char *label;
} menu_row_t;

static int menu_add(menu_row_t *rows, int n, int kind, app_t app, const char *label) {
    if (n < MENU_ROWS_MAX) {
        rows[n].kind = kind;
        rows[n].app = app;
        rows[n].label = label;
        n++;
    }
    return n;
}

/* Case-insensitive: does `hay` contain `needle`. */
static bool ci_contains(const char *hay, const char *needle) {
    size_t nl = strlen(needle), hl = strlen(hay);
    for (size_t i = 0; i + nl <= hl; i++) {
        size_t k = 0;
        while (k < nl && exp_lower(hay[i + k]) == exp_lower(needle[k])) k++;
        if (k == nl) return true;
    }
    return false;
}

/* The programs column. A search replaces the sections with the matching programs. */
static int menu_left_rows(menu_row_t *rows) {
    int n = 0;
    if (g_menu_query_len > 0) {
        for (int a = 0; a < APP_COUNT; a++) {
            if ((app_t)a == APP_SHUTDOWN) continue;
            if (ci_contains(g_app_names[a], g_menu_query)) {
                n = menu_add(rows, n, MROW_ITEM, (app_t)a, g_app_names[a]);
            }
        }
        return n;
    }
    n = menu_add(rows, n, MROW_HEADER, APP_MYCOMP, "Pinned");
    for (unsigned k = 0; k < 4; k++) n = menu_add(rows, n, MROW_ITEM, g_pinned_apps[k], g_app_names[g_pinned_apps[k]]);
    if (g_recent_n > 0) {
        n = menu_add(rows, n, MROW_HEADER, APP_MYCOMP, "Recent");
        for (int k = 0; k < g_recent_n; k++) n = menu_add(rows, n, MROW_ITEM, g_recent[k], g_app_names[g_recent[k]]);
    }
    n = menu_add(rows, n, MROW_HEADER, APP_MYCOMP, "All Programs");
    for (unsigned k = 0; k < 3; k++) n = menu_add(rows, n, MROW_ITEM, g_all_apps[k], g_app_names[g_all_apps[k]]);
    return n;
}

/* The places column, then a rule and Shut Down. */
static int menu_right_rows(menu_row_t *rows) {
    int n = 0;
    for (unsigned k = 0; k < 4; k++) n = menu_add(rows, n, MROW_ITEM, g_place_apps[k], g_app_names[g_place_apps[k]]);
    n = menu_add(rows, n, MROW_SEP, APP_MYCOMP, "");
    n = menu_add(rows, n, MROW_ITEM, APP_SHUTDOWN, g_app_names[APP_SHUTDOWN]);
    return n;
}

static int menu_rows_for(int col, menu_row_t *rows) {
    return col == 0 ? menu_left_rows(rows) : menu_right_rows(rows);
}

static uint32_t menu_col_x(int col) { return col == 0 ? MENU_LEFT_X : MENU_RIGHT_X; }
static uint32_t menu_col_w(int col) { return col == 0 ? MENU_LEFT_CW : MENU_RIGHT_CW; }
static uint32_t menu_top_y(uint32_t sh) { return sh - TASK_H - MENU_H; }
static uint32_t menu_row_y(uint32_t sh, int row) { return menu_top_y(sh) + 8u + (uint32_t)row * MENU_ROW_H; }

/* The k-th choice of a column (headers and rules do not count), as a row index, or -1. */
static int menu_choice_row(const menu_row_t *rows, int n, int k) {
    int seen = 0;
    for (int i = 0; i < n; i++) {
        if (rows[i].kind != MROW_ITEM) continue;
        if (seen == k) return i;
        seen++;
    }
    return -1;
}

static int menu_choice_count(const menu_row_t *rows, int n) {
    int seen = 0;
    for (int i = 0; i < n; i++) if (rows[i].kind == MROW_ITEM) seen++;
    return seen;
}

static void draw_menu_column(const menu_row_t *rows, int n, int col, uint32_t sh) {
    uint32_t x0 = menu_col_x(col), w = menu_col_w(col);
    int choice = 0;
    for (int i = 0; i < n; i++) {
        uint32_t iy = menu_row_y(sh, i);
        if (rows[i].kind == MROW_HEADER) {
            fb_draw_text(x0 + 6u, iy + 5u, rows[i].label, rgb_hex(g_th->heading), 0, false);
            continue;
        }
        if (rows[i].kind == MROW_SEP) {
            fb_fill_rect(x0 + 4u, iy + 9u, w - 8u, 1, rgb_hex(g_th->menu_rim));
            continue;
        }
        bool sel = (col == g_menu_col && choice == g_menu_sel);
        bool hov = (g_hover == HOVER_MENU + col * 50 + i);
        choice++;
        if (sel || hov) fill_box(x0, iy, w, MENU_ROW_H, 3, g_th->menu_hi_top, g_th->menu_hi_bot, true);
        fb_draw_text(x0 + 10u, iy + 5u, rows[i].label,
                     (sel || hov) ? rgb_hex(g_th->menu_hi_text) : c_ink, 0, false);
    }
}

static void draw_start_menu(uint32_t sh) {
    uint32_t my = menu_top_y(sh);
    fill_box(2, my, MENU_W, MENU_H, 6, g_th->menu_rim, g_th->menu_rim, false);          /* outline */
    fill_box(3, my + 1, MENU_W - 2, MENU_H - 2, 5, g_th->menu_top, g_th->menu_bot, false);  /* light panel */
    fb_fill_rect(MENU_LEFT_X + MENU_LEFT_CW + 6u, my + 8u, 1, MENU_H - 16u, rgb_hex(g_th->menu_rim));

    menu_row_t left[MENU_ROWS_MAX], right[MENU_ROWS_MAX];
    int ln = menu_left_rows(left);
    int rn = menu_right_rows(right);
    draw_menu_column(left, ln, 0, sh);
    draw_menu_column(right, rn, 1, sh);

    /* The search box at the foot of the programs column. Typing goes here while the menu is open. */
    uint32_t sy = my + MENU_H - 30u;
    fill_box(MENU_LEFT_X, sy, MENU_LEFT_CW, 22, 3, g_th->menu_rim, g_th->menu_rim, false);
    fill_box(MENU_LEFT_X + 1u, sy + 1u, MENU_LEFT_CW - 2u, 20, 2, c_white, c_white, false);
    if (g_menu_query_len > 0) {
        fb_draw_text(MENU_LEFT_X + 6u, sy + 6u, g_menu_query, c_ink, 0, false);
    } else {
        fb_draw_text(MENU_LEFT_X + 6u, sy + 6u, "Search programs", c_gray, 0, false);
    }
}

/* The desktop's right-click menu: Refresh repaints the desktop; the others open an app. */
static const char *const g_ctx_names[CTX_COUNT] = { "Refresh", "Settings", "About Helix" };

static void draw_context_menu(void) {
    uint32_t h = CTX_COUNT * CTX_ROW + 8u;
    fill_box((uint32_t)g_ctx_x, (uint32_t)g_ctx_y, CTX_W, h, 4, g_th->menu_rim, g_th->menu_rim, false);
    fill_box((uint32_t)g_ctx_x + 1u, (uint32_t)g_ctx_y + 1u, CTX_W - 2u, h - 2u, 3,
             g_th->menu_top, g_th->menu_bot, false);
    for (int i = 0; i < CTX_COUNT; i++) {
        uint32_t iy = (uint32_t)g_ctx_y + 4u + (uint32_t)i * CTX_ROW;
        fb_draw_text((uint32_t)g_ctx_x + 10u, iy + 6u, g_ctx_names[i], c_ink, 0, false);
    }
}

static void draw_taskbar(uint32_t sw, uint32_t sh) {
    uint32_t ty = sh - TASK_H;
    fill_box(0, ty, sw, TASK_H, 0, g_th->task_top, g_th->task_bot, false);   /* dark glass */
    fb_fill_rect(0, ty, sw, 1, rgb_hex(g_th->task_line));

    draw_orb(24, ty + 18, 14, g_menu_open || g_hover == HOVER_ORB);

    /* One button per open window, minimised ones too so they can be brought back. The focused
     * or hovered one is lit; a dot under each button shows the app is running (dim if hidden). */
    uint32_t bx = TB_BTN_X;
    for (int i = 0; i < MAX_WIN; i++) {
        if (!g_win[i].used) continue;
        bool shown = !g_win[i].minimised;
        bool focused = shown && i == g_focus;
        bool hot = g_hover == i;
        if (focused || hot) {
            fill_box(bx, ty + TB_BTN_Y, TB_BTN_W, TB_BTN_H, 4, g_th->tbtn_focus_top, g_th->tbtn_focus_bot, true);
        } else {
            fill_box(bx, ty + TB_BTN_Y, TB_BTN_W, TB_BTN_H, 4, g_th->tbtn_blur_top, g_th->tbtn_blur_bot, true);
        }
        char title[20];
        put_str(title, sizeof(title), g_app_names[g_win[i].app]);
        title[chars_in(TB_BTN_W - 20u)] = '\0';
        uint32_t tcol = (focused || hot) ? rgb_hex(g_th->tbtn_focus_text)
                                         : (shown ? fb_rgb(0xFF, 0xFF, 0xFF) : fb_rgb(0xB8, 0xC8, 0xDC));
        fb_draw_text(bx + 10, ty + TB_BTN_Y + 9, title, tcol, 0, false);
        fb_fill_rect(bx + TB_BTN_W / 2u - 2u, ty + TB_BTN_Y + TB_BTN_H + 2u, 4, 2, shown ? c_white : c_gray);
        bx += TB_BTN_STEP;
    }

    /* Tray: a network and a volume glyph. They are drawn only; they do not read any state yet. */
    uint32_t tray = sw - 150u;
    fb_fill_rect(tray, ty + 11u, 12, 8, c_white);
    fb_fill_rect(tray + 4u, ty + 20u, 4, 2, c_white);
    fb_fill_rect(tray + 22u, ty + 13u, 3, 6, c_white);
    fb_fill_rect(tray + 27u, ty + 11u, 3, 10, c_white);

    /* The clock, shadowed on the glass. */
    int year, month, day, hour, minute, second, weekday;
    time_now_utc(&year, &month, &day, &hour, &minute, &second, &weekday);
    char clock[8];
    clock[0] = (char)('0' + hour / 10);
    clock[1] = (char)('0' + hour % 10);
    clock[2] = ':';
    clock[3] = (char)('0' + minute / 10);
    clock[4] = (char)('0' + minute % 10);
    clock[5] = '\0';
    draw_text_shadow(sw - 92, ty + 14, clock, fb_rgb(0xFF, 0xFF, 0xFF), fb_rgb(0x0B, 0x1F, 0x3A));

    /* Show Desktop: a thin strip at the far right; lit on hover. */
    uint32_t sx = sw - 12u;
    fb_fill_rect(sx - 1u, ty + 4u, 1, TASK_H - 8u, rgb_hex(g_th->task_line));
    if (g_hover == HOVER_SHOW) {
        fill_box(sx, ty, 12, TASK_H, 0, g_th->tbtn_blur_top, g_th->tbtn_blur_bot, false);
    }
}

/* The arrow pointer, 12x19. 'X' is the black outline, '.' the white fill, and a
 * space is transparent (the scene shows through). */
#define CURSOR_W 12
#define CURSOR_H 19
static const char *const g_cursor_rows[CURSOR_H] = {
    "X           ",
    "XX          ",
    "X.X         ",
    "X..X        ",
    "X...X       ",
    "X....X      ",
    "X.....X     ",
    "X......X    ",
    "X.......X   ",
    "X........X  ",
    "X.........X ",
    "X......XXXXX",
    "X..X..X     ",
    "X.X X..X    ",
    "XX  X..X    ",
    "X    X..X   ",
    "     X..X   ",
    "      X..X  ",
    "      XXXX  ",
};

/* The pointer is drawn over the picture, so moving it does not repaint the desktop:
 * the pixels under the arrow are saved first and put back when it moves. */
static uint32_t g_under[CURSOR_H * CURSOR_W];   /* the picture under the arrow */
static int g_cur_x, g_cur_y;                    /* where the arrow was drawn */
static bool g_cur_shown;                        /* is the arrow on screen now */

static void draw_cursor(void) {
    for (uint32_t r = 0; r < CURSOR_H; r++) {
        for (uint32_t c = 0; c < CURSOR_W; c++) {
            char ch = g_cursor_rows[r][c];
            if (ch == 'X') {
                fb_fill_rect((uint32_t)g_mx + c, (uint32_t)g_my + r, 1, 1, c_black);
            } else if (ch == '.') {
                fb_fill_rect((uint32_t)g_mx + c, (uint32_t)g_my + r, 1, 1, c_white);
            }
        }
    }
}

/* Put back the picture under the arrow, if it is on screen. */
static void cursor_restore(void) {
    if (!g_cur_shown) return;
    for (uint32_t r = 0; r < CURSOR_H; r++) {
        for (uint32_t c = 0; c < CURSOR_W; c++) {
            fb_fill_rect((uint32_t)g_cur_x + c, (uint32_t)g_cur_y + r, 1, 1,
                         g_under[r * CURSOR_W + c]);
        }
    }
    g_cur_shown = false;
}

/* Save the picture under the arrow at (g_mx, g_my), then draw the arrow. */
static void cursor_show(void) {
    g_cur_x = g_mx;
    g_cur_y = g_my;
    for (uint32_t r = 0; r < CURSOR_H; r++) {
        for (uint32_t c = 0; c < CURSOR_W; c++) {
            g_under[r * CURSOR_W + c] = fb_get_pixel((uint32_t)g_mx + c, (uint32_t)g_my + r);
        }
    }
    draw_cursor();
    g_cur_shown = true;
}

/* Move the arrow only: a small change to the screen, no repaint. */
static void cursor_move(int nx, int ny) {
    cursor_restore();
    g_mx = nx;
    g_my = ny;
    cursor_show();
}

/* Draw or erase the outline of a w x h box at (x, y). Each pixel is XORed with
 * the mask, so calling this twice with the same arguments restores the screen. */
static void outline_xor_px(uint32_t x, uint32_t y) {
    fb_fill_rect(x, y, 1, 1, fb_get_pixel(x, y) ^ c_outline);
}

static void outline_xor(int x, int y, uint32_t w, uint32_t h) {
    if (w < 2 || h < 2) return;
    for (uint32_t i = 0; i < w; i++) {
        outline_xor_px((uint32_t)x + i, (uint32_t)y);
        outline_xor_px((uint32_t)x + i, (uint32_t)y + h - 1);
    }
    for (uint32_t j = 1; j + 1 < h; j++) {
        outline_xor_px((uint32_t)x, (uint32_t)y + j);
        outline_xor_px((uint32_t)x + w - 1, (uint32_t)y + j);
    }
}

/* Move the drag outline so the grab point stays under the pointer at (nx, ny).
 * The window's own pixels are not touched. */
static void drag_move(int nx, int ny) {
    window_t *w = &g_win[g_drag];
    uint32_t sw = fb_width(), sh = fb_height();
    cursor_restore();                           /* the arrow's saved pixels must not hold the outline */
    if (g_outline_shown) outline_xor(g_out_x, g_out_y, w->w, w->h);
    int wx = nx - g_drag_dx;
    int wy = ny - g_drag_dy;
    int max_x = (int)sw - (int)w->w;
    int max_y = (int)sh - (int)TASK_H - (int)w->h;
    if (wx < 0) wx = 0;
    if (wy < 0) wy = 0;
    if (wx > max_x) wx = max_x;
    if (wy > max_y) wy = max_y;
    g_out_x = wx;
    g_out_y = wy;
    outline_xor(g_out_x, g_out_y, w->w, w->h);
    g_outline_shown = true;
    g_mx = nx;
    g_my = ny;
    cursor_show();
}

/* The resize outline follows the pointer. The grip is the bottom-right corner, so the top-left
 * corner stays where it is. Sizes are clamped to a minimum, so the title buttons stay usable,
 * and to the screen above the taskbar. */
static void resize_move(int nx, int ny) {
    uint32_t sw = fb_width(), sh = fb_height();
    cursor_restore();
    if (g_outline_shown) outline_xor(g_out_x, g_out_y, g_rs_w, g_rs_h);
    int rw = nx - g_out_x + 1;
    int rh = ny - g_out_y + 1;
    int max_w = (int)sw - g_out_x;
    int max_h = (int)(sh - TASK_H) - g_out_y;
    if (rw < 200) rw = 200;
    if (rh < 120) rh = 120;
    if (rw > max_w) rw = max_w;
    if (rh > max_h) rh = max_h;
    g_rs_w = rw;
    g_rs_h = rh;
    outline_xor(g_out_x, g_out_y, g_rs_w, g_rs_h);
    g_outline_shown = true;
    g_mx = nx;
    g_my = ny;
    cursor_show();
}

/* Stacking order: the windows are drawn in slot order, except that the focused window is
 * drawn last, so it is on top. Clicks test the same order from the top down. k is the
 * position in that order (0 is the bottom). */
static int z_slot(int k) {
    if (g_focus < 0 || g_focus == MAX_WIN - 1) return k;
    if (k == MAX_WIN - 1) return g_focus;
    return k < g_focus ? k : k + 1;
}

static void redraw(void) {
    uint32_t sw = fb_width(), sh = fb_height();
    g_cur_shown = false;                        /* the frame below wipes the old arrow */
    g_outline_shown = false;
    if (!fb_draw_wallpaper(g_th->desk_top, g_th->desk_bot)) {          /* sky-blue wallpaper */
        fill_box(0, 0, sw, sh, 0, g_th->desk_top, g_th->desk_bot, false);
    }
    for (int i = 0; i < ICON_COUNT; i++) {
        draw_icon(i, 16, 16u + (uint32_t)i * 72u, i == g_icon_sel && !g_menu_open);
    }
    for (int k = 0; k < MAX_WIN; k++) {
        int i = z_slot(k);
        if (g_win[i].used && !g_win[i].minimised) draw_window(&g_win[i], i == g_focus);
    }
    if (g_drag >= 0 && g_win[g_drag].used) {   /* a drag in progress: keep its outline */
        outline_xor(g_out_x, g_out_y, g_win[g_drag].w, g_win[g_drag].h);
        g_outline_shown = true;
    }
    if (g_resize >= 0) {                        /* a resize in progress: keep its outline */
        outline_xor(g_out_x, g_out_y, g_rs_w, g_rs_h);
        g_outline_shown = true;
    }
    draw_taskbar(sw, sh);
    if (g_menu_open) draw_start_menu(sh);
    if (g_ctx_open) draw_context_menu();
    cursor_show();
}

/* Set by handle_key: false when the key was drawn in place and the frame needs no repaint. */
static bool g_repaint;

static bool rect_hits(int x, int y, int w, int h, const window_t *o) {
    return x < (int)(o->x + o->w) && x + w > (int)o->x &&
           y < (int)(o->y + o->h) && y + h > (int)o->y;
}

/* Typing in the terminal redraws only the cells that changed on the input row,
 * the way the shell echoes a character, instead of repainting the whole desktop.
 * It falls back to a full redraw when the row could be covered by another window,
 * a menu is open, or a drag outline is on screen. */
static void term_redraw_input(int old_len) {
    if (g_focus < 0 || !g_win[g_focus].used || g_win[g_focus].app != APP_TERMINAL) {
        redraw();
        return;
    }
    window_t *w = &g_win[g_focus];
    uint32_t cx = w->x + 6, cy = w->y + 26, cw = w->w - 12, ch = w->h - 32;
    int rows = (int)((ch - 8u) / 12u);
    int total = g_term_n + 1;                      /* the buffer lines, then the input line */
    int shown = total > rows ? rows : total;
    int row_y = (int)(cy + 4u) + (shown - 1) * 12;

    int lo = old_len < g_term_in_len ? old_len : g_term_in_len;   /* first cell that changed */
    int hi = old_len > g_term_in_len ? old_len : g_term_in_len;   /* last input position used */
    int x0 = (int)(cx + 4u) + (TERM_PROMPT_LEN + lo) * 8;   /* where the first changed cell is drawn */
    int x1 = (int)(cx + 4u) + (TERM_PROMPT_LEN + hi + 2) * 8;    /* caret and one blank cell */
    if (x1 > (int)(cx + cw)) x1 = (int)(cx + cw);

    bool covered = g_menu_open || g_drag >= 0 || g_resize >= 0;
    for (int i = g_focus + 1; i < MAX_WIN && !covered; i++) {
        if (g_win[i].used && rect_hits(x0, row_y, x1 - x0, 12, &g_win[i])) covered = true;
    }
    if (covered) {
        redraw();
        return;
    }

    /* The row, as drawn by term_draw: prompt, the typed text, the caret, a blank cell. */
    char row[TERM_COLS + 4];
    int k = 0;
    for (const char *p = TERM_PROMPT; *p != '\0'; p++) row[k++] = *p;
    for (int i = 0; i < g_term_in_len; i++) row[k++] = g_term_in[i];
    row[k++] = '_';
    row[k++] = ' ';
    row[k] = '\0';

    cursor_restore();                              /* the arrow's saved pixels must not go stale */
    /* Start the draw at column prompt + lo, the same cell term_draw puts that character in. */
    fb_draw_text((uint32_t)(x0), (uint32_t)row_y, row + TERM_PROMPT_LEN + lo, c_white, c_black, true);
    cursor_show();
}

/* ------------------------------------------------------------------ */
/* Windows and input                                                   */
/* ------------------------------------------------------------------ */

static void focus_next(void) {
    for (int step = 1; step <= MAX_WIN; step++) {
        int i = (g_focus + step) % MAX_WIN;
        if (g_win[i].used && !g_win[i].minimised) {
            g_focus = i;
            return;
        }
    }
}

/* Remember the app as the most recent, newest first, at most three. */
static void record_recent(app_t app) {
    int at = -1;
    for (int k = 0; k < g_recent_n; k++) if (g_recent[k] == app) at = k;
    if (at < 0) {                       /* a new app: make room at the front, dropping the oldest */
        if (g_recent_n < 3) g_recent_n++;
        at = g_recent_n - 1;
    }
    for (int k = at; k > 0; k--) g_recent[k] = g_recent[k - 1];
    g_recent[0] = app;
}

static void open_app(app_t app) {
    record_recent(app);
    for (int i = 0; i < MAX_WIN; i++) {
        if (g_win[i].used && g_win[i].app == app) {
            g_win[i].minimised = false;   /* already open: bring it back and forward */
            g_focus = i;
            return;
        }
    }
    for (int i = 0; i < MAX_WIN; i++) {
        if (g_win[i].used) continue;
        uint32_t n = (uint32_t)i;
        g_win[i].used = true;
        g_win[i].app = app;
        g_win[i].minimised = false;
        g_win[i].maximised = false;
        g_win[i].x = 120u + n * 28u;
        g_win[i].y = 40u + n * 28u;
        g_win[i].w = app == APP_TERMINAL ? 480 : app == APP_EDITOR ? 420 : app == APP_BROWSER ? 460 :
                     app == APP_MYCOMP ? 520 : app == APP_NETWORK ? 420 : 380;
        g_win[i].h = app == APP_TERMINAL ? 330 : app == APP_EDITOR ? 280 : app == APP_BROWSER ? 340 :
                     app == APP_MYCOMP ? 320 : app == APP_SETTINGS ? 300 :
                     app == APP_NETWORK ? 280 : 250;
        g_focus = i;
        build_content(app);
        if (app == APP_MYCOMP) exp_init();   /* opens at the root of the volume */
    if (app == APP_NETWORK && g_net.out_n == 0) net_out("Type a host name or address, then press Enter.");
        if (app == APP_EDITOR) editor_load_file(&g_ed);   /* once; the text survives reopening */
        if (app == APP_TERMINAL && !g_term_started) {
            g_term_started = true;
            g_term_n = 1;
            g_term_lines[0][0] = '\0';
            term_welcome();
        }
        klog_note(KLOG_DESKTOP, "[desktop] opened %s\n", g_app_names[app]);
        return;
    }
}

static void close_window(int i) {
    if (i < 0 || !g_win[i].used) return;
    klog_note(KLOG_DESKTOP, "[desktop] closed %s\n", g_app_names[g_win[i].app]);
    g_win[i].used = false;
    g_focus = -1;
    for (int k = 0; k < MAX_WIN; k++) {
        if (g_win[k].used && !g_win[k].minimised) {
            g_focus = k;
            break;
        }
    }
}

static void close_focused(void) {
    close_window(g_focus);
}

static bool inside(int x, int y, uint32_t rx, uint32_t ry, uint32_t rw, uint32_t rh) {
    return x >= (int)rx && y >= (int)ry && x < (int)(rx + rw) && y < (int)(ry + rh);
}

/* A click in My Computer: Up goes up; a list row selects it, and a folder that is
 * already selected opens. The preview pane takes no clicks. */
static void exp_click(const window_t *w, int x, int y) {
    uint32_t cx = w->x + 6u, cy = w->y + 26u, cw = w->w - 12u, ch = w->h - 32u;
    (void)cw;
    if (y < (int)(cy + 26u)) {
        if (inside(x, y, cx + 4u, cy + 3u, 40u, 20u)) exp_up();
        return;
    }
    if (y < (int)(cy + EXP_TOOL_H)) {
        uint32_t by = cy + EXP_BTN_Y;
        if (inside(x, y, cx + 4u, by, 84u, EXP_BTN_H)) exp_new_item(true);
        else if (inside(x, y, cx + 92u, by, 76u, EXP_BTN_H)) exp_new_item(false);
        else if (inside(x, y, cx + 172u, by, 56u, EXP_BTN_H)) exp_copy_selected();
        else if (inside(x, y, cx + 232u, by, 56u, EXP_BTN_H)) exp_edit_selected();
        else if (inside(x, y, cx + 292u, by, 64u, EXP_BTN_H)) exp_delete_selected();
        return;
    }
    uint32_t list_y = cy + EXP_TOOL_H + 2u;
    if (x < (int)cx || x >= (int)(cx + EXP_LIST_W) || y < (int)list_y) return;
    uint32_t row = ((uint32_t)y - list_y) / 12u;
    if (row >= exp_rows(ch)) return;
    uint32_t i = g_exp.top + row;
    if (i >= g_exp.n) return;
    if (i == g_exp.sel && g_exp.ent[i].dir) {
        exp_open(i);
    } else {
        exp_select(i);
    }
}

/* A click in the Network window: Ping, or Lookup alone. */
static void network_click(const window_t *w, int x, int y) {
    uint32_t cx = w->x + 6u, cy = w->y + 26u;
    if (inside(x, y, cx + 52u, cy + 34u, 64u, 22u)) {
        net_run(true);
    } else if (inside(x, y, cx + 124u, cy + 34u, 84u, 22u)) {
        net_run(false);
    }
}

/* A click on the Settings page: the Appearance button flips the theme. */
static void settings_click(const window_t *w, int x, int y) {
    uint32_t cx = w->x + 6u, cy = w->y + 26u;
    if (inside(x, y, cx + 10u, cy + 24u, SET_THEME_W, SET_THEME_H)) {
        g_theme_idx = (g_theme_idx + 1u) % THEME_COUNT;
        theme_apply();
    }
}

/* Keys in My Computer: Up/Down move the selection (a file's preview follows), Enter
 * opens, Backspace goes up. Returns false for keys it does not use (Esc, Tab, x, s). */
static bool exp_key(int c) {
    if (c == SHELL_KEY_UP) {
        if (g_exp.sel > 0) exp_select(g_exp.sel - 1u);
        return true;
    }
    if (c == SHELL_KEY_DOWN) {
        if (g_exp.sel + 1u < g_exp.n) exp_select(g_exp.sel + 1u);
        return true;
    }
    if (c == '\n') {
        exp_open(g_exp.sel);
        return true;
    }
    if (c == 0x08 || c == 0x7f) {
        exp_up();
        return true;
    }
    /* The toolbar's buttons, by key: n new folder, f new file, c copy, d delete (press twice), e edit. */
    if (c == 'n' || c == 'N') { exp_new_item(true); return true; }
    if (c == 'f' || c == 'F') { exp_new_item(false); return true; }
    if (c == 'c' || c == 'C') { exp_copy_selected(); return true; }
    if (c == 'd' || c == 'D') { exp_delete_selected(); return true; }
    if (c == 'e' || c == 'E') { exp_edit_selected(); return true; }
    return false;
}

/* A left click at (x, y). Returns PTR_EXIT for Shut Down, otherwise PTR_REDRAW. */
/* A click inside the editor: the Save button saves, a click in the text moves the caret. */
static void editor_click(const window_t *w, int x, int y) {
    uint32_t cx = w->x + 6u, cy = w->y + 26u, cw = w->w - 12u, ch = w->h - 32u;
    uint32_t sy = cy + ch - ED_STRIP_H;
    if (inside(x, y, cx + cw - ED_SAVE_W - 4u, sy + 3u, ED_SAVE_W, 16)) {
        editor_save(&g_ed);
        return;
    }
    if (y < (int)(cy + 4u) || y >= (int)sy) return;
    uint32_t row = ((uint32_t)y - cy - 4u) / 12u;
    int px = x - (int)cx - 4 + 4;             /* +4 rounds to the nearest character */
    if (px < 0) px = 0;
    editor_goto(&g_ed, g_ed.top + row, (uint32_t)px / 8u);
}

static void editor_redraw(void);

static void browser_redraw(void);

/* A click on the browser's scrollbar: an arrow steps one row, the track above or below
 * the thumb pages, and the thumb itself is not dragged yet. Returns true when the click
 * was on the bar, so the caller stops there. */
static bool browser_bar_click(const window_t *w, int x, int y) {
    uint32_t cx = w->x + 6u, cy = w->y + 26u, cw = w->w - 12u;
    uint32_t py = cy + BR_BAR_H + 12u;
    uint32_t bx = cx + cw - 6u - BR_BAR_W;
    uint32_t bar_h = BR_VISIBLE * 12u;
    if (x < (int)bx || x >= (int)(bx + BR_BAR_W) || y < (int)py || y >= (int)(py + bar_h)) return false;

    uint32_t rows = br_layout(false, 0, 0, 0);
    uint32_t max_top = rows > BR_VISIBLE ? rows - BR_VISIBLE : 0;
    uint32_t track_y, track_h, thumb_y, thumb_h;
    br_bar_geometry(py, rows, &track_y, &track_h, &thumb_y, &thumb_h);
    uint32_t uy = (uint32_t)y;
    if (uy < py + BR_ARROW) {
        if (g_br.top > 0) g_br.top--;
    } else if (uy >= py + bar_h - BR_ARROW) {
        if (g_br.top < max_top) g_br.top++;
    } else if (uy < thumb_y) {
        g_br.top = g_br.top > BR_VISIBLE ? g_br.top - BR_VISIBLE : 0;
    } else if (uy >= thumb_y + thumb_h) {
        uint32_t next = g_br.top + BR_VISIBLE;
        g_br.top = next < max_top ? next : max_top;
    }
    return true;
}

/* A click at (x, y). Returns PTR_EXIT for Shut Down, PTR_REDRAW when the picture
 * needs a full repaint, and PTR_NONE when nothing visible changed (or it was
 * already repainted in place). */
/* The topmost window that is shown (not minimised), or -1. */
static int pick_focus(void) {
    for (int k = MAX_WIN - 1; k >= 0; k--) {
        int i = z_slot(k);
        if (g_win[i].used && !g_win[i].minimised) return i;
    }
    return -1;
}

static void window_minimise(int i) {
    g_win[i].minimised = true;
    klog_note(KLOG_DESKTOP, "[desktop] minimised %s\n", g_app_names[g_win[i].app]);
    g_focus = pick_focus();
}

/* Maximise fills the screen above the taskbar; a second press puts the window back. */
static void window_toggle_max(int i) {
    window_t *w = &g_win[i];
    if (w->maximised) {
        w->x = w->rx;
        w->y = w->ry;
        w->w = w->rw;
        w->h = w->rh;
        w->maximised = false;
    } else {
        w->rx = w->x;
        w->ry = w->y;
        w->rw = w->w;
        w->rh = w->h;
        w->x = 0;
        w->y = 0;
        w->w = fb_width();
        w->h = fb_height() - TASK_H;
        w->maximised = true;
    }
    g_focus = i;
}

/* A taskbar button: restores a hidden window, hides the focused one, or focuses another. */
static int taskbar_button(int i) {
    if (g_win[i].minimised) {
        g_win[i].minimised = false;
        g_focus = i;
        return PTR_REDRAW;
    }
    if (g_focus == i) {
        window_minimise(i);
        return PTR_REDRAW;
    }
    g_focus = i;
    return PTR_REDRAW;
}

/* Show Desktop: hides every shown window; when none is shown, brings them all back. */
static void show_desktop(void) {
    bool any_shown = false;
    for (int i = 0; i < MAX_WIN; i++) if (g_win[i].used && !g_win[i].minimised) any_shown = true;
    if (any_shown) {
        for (int i = 0; i < MAX_WIN; i++) {
            if (g_win[i].used && !g_win[i].minimised) g_win[i].minimised = true;
        }
        g_focus = -1;
    } else {
        for (int i = 0; i < MAX_WIN; i++) if (g_win[i].used) g_win[i].minimised = false;
        g_focus = pick_focus();
    }
}

/* Runs a Start menu choice. Returns true when it is Shut Down (the desktop should stop). */
static bool menu_run_item(app_t app) {
    g_menu_open = false;
    g_menu_query_len = 0;
    g_menu_query[0] = '\0';
    if (app == APP_SHUTDOWN) return true;
    open_app(app);
    return false;
}

/* The keyboard's current choice in the column that has focus. */
static bool menu_selected_app(app_t *out) {
    menu_row_t rows[MENU_ROWS_MAX];
    int n = menu_rows_for(g_menu_col, rows);
    int r = menu_choice_row(rows, n, g_menu_sel);
    if (r < 0) return false;
    *out = rows[r].app;
    return true;
}

/* Arrow keys move through the choices of the column, wrapping at the ends. */
static void menu_move(int d) {
    menu_row_t rows[MENU_ROWS_MAX];
    int n = menu_choice_count(rows, menu_rows_for(g_menu_col, rows));
    if (n == 0) {
        g_menu_sel = 0;
        return;
    }
    g_menu_sel = (g_menu_sel + d + n) % n;
}

/* The Start menu row under (x, y): HOVER_MENU + column * 50 + row, or HOVER_NONE. */
static int menu_row_at(int x, int y, uint32_t sh, app_t *app_out) {
    for (int col = 0; col < 2; col++) {
        menu_row_t rows[MENU_ROWS_MAX];
        int n = menu_rows_for(col, rows);
        for (int i = 0; i < n; i++) {
            if (rows[i].kind != MROW_ITEM) continue;
            if (inside(x, y, menu_col_x(col), menu_row_y(sh, i), menu_col_w(col), MENU_ROW_H)) {
                *app_out = rows[i].app;
                return HOVER_MENU + col * 50 + i;
            }
        }
    }
    return HOVER_NONE;
}

static int ctx_item_at(int x, int y) {
    if (!g_ctx_open) return -1;
    for (int i = 0; i < CTX_COUNT; i++) {
        if (inside(x, y, (uint32_t)g_ctx_x, (uint32_t)g_ctx_y + 4u + (uint32_t)i * CTX_ROW, CTX_W, CTX_ROW)) return i;
    }
    return -1;
}

/* Refresh (item 0) only repaints; Settings and About Helix open their windows. */
static int ctx_run(int item) {
    if (item == 1) open_app(APP_SETTINGS);
    if (item == 2) open_app(APP_ABOUT);
    return PTR_REDRAW;
}

/* A right click on empty desktop opens the desktop menu. Over a window, an icon, the taskbar
 * or while the Start menu is open, it only closes the menu. */
static int handle_right_click(int x, int y, uint32_t sh) {
    uint32_t sw = fb_width();
    if (g_menu_open) {
        g_menu_open = false;
        return PTR_REDRAW;
    }
    if (y >= (int)(sh - TASK_H)) return PTR_NONE;
    for (int k = MAX_WIN - 1; k >= 0; k--) {
        int i = z_slot(k);
        if (g_win[i].used && !g_win[i].minimised &&
            inside(x, y, g_win[i].x, g_win[i].y, g_win[i].w, g_win[i].h)) return PTR_NONE;
    }
    for (int i = 0; i < ICON_COUNT; i++) {
        if (inside(x, y, 12, 16u + (uint32_t)i * 72u, 100, 52)) return PTR_NONE;
    }
    int maxx = (int)sw - (int)CTX_W - 2;
    int maxy = (int)(sh - TASK_H) - (int)(CTX_COUNT * CTX_ROW + 8u) - 2;
    g_ctx_x = x > maxx ? maxx : x;
    g_ctx_y = y > maxy ? maxy : y;
    g_ctx_open = true;
    return PTR_REDRAW;
}

/* What the pointer is over, for the hover highlight. */
static int hover_at(int x, int y, uint32_t sw, uint32_t sh) {
    if (g_ctx_open) return HOVER_NONE;
    if (g_menu_open) {
        app_t app;
        int r = menu_row_at(x, y, sh, &app);
        if (r != HOVER_NONE) return r;
    }
    uint32_t ty = sh - TASK_H;
    if (y < (int)ty) return HOVER_NONE;
    if (inside(x, y, 4, ty + 3, 40, 30)) return HOVER_ORB;
    if (inside(x, y, sw - 12u, ty, 12, TASK_H)) return HOVER_SHOW;
    uint32_t bx = TB_BTN_X;
    for (int i = 0; i < MAX_WIN; i++) {
        if (!g_win[i].used) continue;
        if (inside(x, y, bx, ty + TB_BTN_Y, TB_BTN_W, TB_BTN_H)) return i;
        bx += TB_BTN_STEP;
    }
    return HOVER_NONE;
}

static int handle_click(int x, int y, uint32_t sh) {
    uint32_t sw = fb_width();
    uint32_t ty = sh - TASK_H;
    bool menu_was_open = g_menu_open;

    if (g_ctx_open) {                  /* a click closes the desktop menu; a choice in it runs */
        int item = ctx_item_at(x, y);
        g_ctx_open = false;
        if (item >= 0) return ctx_run(item);
        return PTR_REDRAW;
    }
    if (g_menu_open) {
        app_t app;
        if (menu_row_at(x, y, sh, &app) != HOVER_NONE) {
            return menu_run_item(app) ? PTR_EXIT : PTR_REDRAW;
        }
        if (inside(x, y, 2, menu_top_y(sh), MENU_W, MENU_H)) return PTR_NONE;   /* the panel itself */
        g_menu_open = false;                                   /* any other click closes it */
        g_menu_query_len = 0;
        g_menu_query[0] = '\0';
    }

    if (inside(x, y, 4, ty + 3, 40, 30)) {                     /* Start orb */
        g_menu_open = !g_menu_open;
        g_menu_sel = 0;
        g_menu_col = 0;
        g_menu_query_len = 0;
        g_menu_query[0] = '\0';
        return PTR_REDRAW;
    }
    if (inside(x, y, sw - 12u, ty, 12, TASK_H)) {              /* Show Desktop */
        show_desktop();
        return PTR_REDRAW;
    }

    uint32_t bx = TB_BTN_X;                                    /* taskbar window buttons */
    for (int i = 0; i < MAX_WIN; i++) {
        if (!g_win[i].used) continue;
        if (inside(x, y, bx, ty + TB_BTN_Y, TB_BTN_W, TB_BTN_H)) return taskbar_button(i);
        bx += TB_BTN_STEP;
    }
    if (y >= (int)ty) return menu_was_open ? PTR_REDRAW : PTR_NONE;   /* the rest of the taskbar */

    /* Windows, topmost first: the title buttons, then the title bar (drag), then the body. */
    for (int k = MAX_WIN - 1; k >= 0; k--) {
        int i = z_slot(k);
        if (!g_win[i].used || g_win[i].minimised) continue;
        window_t *w = &g_win[i];
        if (inside(x, y, w->x + w->w - 21, w->y + 5, 16, 14)) {
            close_window(i);
            return PTR_REDRAW;
        }
        if (inside(x, y, w->x + w->w - 39, w->y + 5, 16, 14)) {
            window_toggle_max(i);
            return PTR_REDRAW;
        }
        if (inside(x, y, w->x + w->w - 57, w->y + 5, 16, 14)) {
            window_minimise(i);
            return PTR_REDRAW;
        }
        if (inside(x, y, w->x, w->y, w->w, 24)) {          /* title bar: drag, unless maximised */
            g_focus = i;
            uint64_t now = time_uptime_ms();
            if (g_last_title_win == i && now - g_last_title_ms < 500) {   /* double-click */
                g_last_title_win = -1;
                window_toggle_max(i);
                return PTR_REDRAW;
            }
            g_last_title_win = i;
            g_last_title_ms = now;
            if (w->maximised) return PTR_REDRAW;
            g_drag = i;
            g_drag_dx = x - (int)w->x;
            g_drag_dy = y - (int)w->y;
            g_out_x = (int)w->x;
            g_out_y = (int)w->y;
            klog_event(KLOG_DESKTOP, "drag start %s", g_app_names[w->app]);
            return PTR_REDRAW;
        }
        if (!w->maximised && inside(x, y, w->x + w->w - 14u, w->y + w->h - 14u, 14, 14)) {   /* resize grip */
            g_focus = i;
            g_resize = i;
            g_out_x = (int)w->x;
            g_out_y = (int)w->y;
            g_rs_w = (int)w->w;
            g_rs_h = (int)w->h;
            g_outline_shown = false;
            return PTR_REDRAW;
        }
        if (inside(x, y, w->x, w->y, w->w, w->h)) {
            bool refocus = g_focus != i;
            g_focus = i;
            if (w->app == APP_EDITOR) editor_click(w, x, y);
            if (w->app == APP_BROWSER && !refocus && !menu_was_open && browser_bar_click(w, x, y)) {
                browser_redraw();                 /* repaints the window, or asks for a full repaint */
                bool full = g_repaint;
                g_repaint = false;
                return full ? PTR_REDRAW : PTR_NONE;
            }
            if (w->app == APP_NETWORK && !refocus && !menu_was_open) {
                network_click(w, x, y);
                return PTR_REDRAW;
            }
            if (w->app == APP_SETTINGS && !refocus && !menu_was_open) {
                settings_click(w, x, y);
                return PTR_REDRAW;                 /* the theme changed: repaint everything */
            }
            if (w->app == APP_MYCOMP && !refocus && !menu_was_open) {
                exp_click(w, x, y);                /* the list or Up changed: repaint the window */
                return PTR_REDRAW;
            }
            if (refocus || menu_was_open) return PTR_REDRAW;   /* title colours or the menu changed */
            if (w->app == APP_EDITOR) {                        /* caret moved or Save pressed */
                editor_redraw();
                bool full = g_repaint;
                g_repaint = false;
                return full ? PTR_REDRAW : PTR_NONE;
            }
            return PTR_NONE;                                   /* body of the focused window: nothing to do */
        }
    }

    for (int i = 0; i < ICON_COUNT; i++) {                     /* desktop icons */
        uint32_t iy = 16u + (uint32_t)i * 72u;
        if (inside(x, y, 12, iy, 100, 52)) {
            g_icon_sel = i;
            open_app(g_icon_app[i]);
            return PTR_REDRAW;
        }
    }
    return menu_was_open ? PTR_REDRAW : PTR_NONE;              /* only closing the menu changed anything */
}

/* Read the mouse. Moves the pointer by the accumulated delta and reports a left
 * click (button press edge). Safe to call when no mouse is attached. */
static int poll_pointer(void) {
    usb_host_poll();
    usb_hid_mouse_t m;
    /* USB mouse first, then the PS/2 mouse (QEMU's default pointer). */
    bool moved = usb_hid_get_mouse_event(&m) || ps2_mouse_get_event(&m);
    uint32_t sw = fb_width(), sh = fb_height();
    int result = PTR_NONE;
    /* The button state comes from the packet. With no new packet, m is not written,
     * so keep the last state rather than reading stale bytes as a release. */
    bool down = moved ? ((m.buttons & 1u) != 0) : g_button_was_down;
    bool right = moved ? ((m.buttons & 2u) != 0) : g_right_was_down;
    bool hover_changed = false;

    if (moved) {
        int nx = g_mx + m.dx;
        int ny = g_my + m.dy;
        if (nx < 0) nx = 0;
        if (ny < 0) ny = 0;
        if (nx > (int)sw - 1) nx = (int)sw - 1;
        if (ny > (int)sh - 1) ny = (int)sh - 1;
        klog_event(KLOG_USB, "mouse dx=%d dy=%d buttons=%x -> %d,%d", (int)m.dx, (int)m.dy,
                   (unsigned)m.buttons, nx, ny);
        if (g_drag >= 0 && down) {
            drag_move(nx, ny);                 /* only the outline moves; no repaint */
        } else if (g_resize >= 0 && down) {
            resize_move(nx, ny);
        } else {
            cursor_move(nx, ny);               /* plain move: only the arrow changes */
            int hv = hover_at(g_mx, g_my, sw, sh);
            if (hv != g_hover) {
                g_hover = hv;
                hover_changed = true;
            }
        }
    }

    if (!down && g_button_was_down && g_drag >= 0) {
        /* Release: the window goes where the outline is, and is drawn in full. */
        if (g_win[g_drag].used) {
            g_win[g_drag].x = (uint32_t)g_out_x;
            g_win[g_drag].y = (uint32_t)g_out_y;
        }
        g_drag = -1;
        g_outline_shown = false;
        result = PTR_REDRAW;
    }
    if (!down && g_button_was_down && g_resize >= 0) {
        /* Release: the window takes the outlined size, and a full repaint clears the outline. */
        if (g_win[g_resize].used) {
            g_win[g_resize].w = (uint32_t)g_rs_w;
            g_win[g_resize].h = (uint32_t)g_rs_h;
        }
        g_resize = -1;
        g_outline_shown = false;
        result = PTR_REDRAW;
    }
    if (down && !g_button_was_down) {
        klog_event(KLOG_INPUT, "click at %d,%d", g_mx, g_my);
        result = handle_click(g_mx, g_my, sh);
    }
    if (right && !g_right_was_down) {
        int r = handle_right_click(g_mx, g_my, sh);
        if (r != PTR_NONE) result = r;
    }
    g_button_was_down = down;
    g_right_was_down = right;

    if (hover_changed && result == PTR_NONE) {
        /* Only the taskbar and the open menus show hover: repaint those, not the desktop. */
        cursor_restore();
        draw_taskbar(sw, sh);
        if (g_menu_open) draw_start_menu(sh);
        if (g_ctx_open) draw_context_menu();
        cursor_show();
    }
    return result;
}

/* Repaint only the editor rows whose text or caret changed since the last paint, and the
 * status strip if its text changed. Nothing else is touched, so typing does not flash the
 * frame, title bar or the rest of the text. Returns false when the picture on screen does
 * not match what was last drawn (first paint, or the window moved): the caller then
 * repaints the whole window. The arrow must already be restored. */
static bool editor_update_rows(const window_t *w) {
    uint32_t cx = w->x + 6u, cy = w->y + 26u, cw = w->w - 12u, ch = w->h - 32u;
    if (!g_ed_shown.valid || cx != g_ed_shown.cx || cy != g_ed_shown.cy ||
        cw != g_ed_shown.cw || ch != g_ed_shown.ch) {
        return false;
    }
    uint32_t rows = ed_row_count(ch);
    editor_scroll_to_cursor(&g_ed, rows);       /* may scroll; the row comparison catches that */

    static char now[ED_MAX_ROWS][ED_MAX_COLS + 1];
    int caret_row;
    uint32_t caret_off;
    ed_layout(rows, ed_col_count(cw), now, &caret_row, &caret_off);

    bool caret_moved = caret_row != g_ed_shown.caret_row || caret_off != g_ed_shown.caret_off;
    for (uint32_t r = 0; r < rows; r++) {
        bool text_changed = strcmp(now[r], g_ed_shown.rows[r]) != 0;
        bool caret_here = (int)r == caret_row || (int)r == g_ed_shown.caret_row;
        if (!text_changed && !(caret_moved && caret_here)) continue;
        fb_fill_rect(cx, cy + 4u + r * 12u, cw, 12u, c_client);      /* just this row's band */
        ed_paint_row(cx, cy, cw, r, now[r], (int)r == caret_row, caret_off);
        memcpy(g_ed_shown.rows[r], now[r], sizeof now[r]);
    }
    g_ed_shown.caret_row = caret_row;
    g_ed_shown.caret_off = caret_off;

    if (strcmp(g_ed.msg, g_ed_shown.msg) != 0) {
        ed_draw_strip(cx, cy, cw, ch);
        memcpy(g_ed_shown.msg, g_ed.msg, sizeof g_ed_shown.msg);
    }
    return true;
}

/* Redraw only the editor window after its text changed. The desktop, icons and
 * taskbar stay as they are. Falls back to the full redraw when another window
 * is on top of the editor, a menu or drag is up, or the window reaches the taskbar. */
static void editor_redraw(void) {
    int i = g_focus;
    if (i < 0 || !g_win[i].used || g_win[i].app != APP_EDITOR || g_menu_open || g_drag >= 0 ||
        g_win[i].y + g_win[i].h > fb_height() - TASK_H) {
        g_repaint = true;
        return;
    }
    window_t *w = &g_win[i];
    for (int k = i + 1; k < MAX_WIN; k++) {
        if (g_win[k].used && rect_hits((int)w->x, (int)w->y, (int)w->w, (int)w->h, &g_win[k])) {
            g_repaint = true;
            return;
        }
    }
    cursor_restore();
    if (!editor_update_rows(w)) draw_window(w, true);
    cursor_show();
    g_repaint = false;
}

/* Redraw only the browser window, as editor_redraw does for the editor. */
static void browser_redraw(void) {
    int i = g_focus;
    if (i < 0 || !g_win[i].used || g_win[i].app != APP_BROWSER || g_menu_open || g_drag >= 0 ||
        g_win[i].y + g_win[i].h > fb_height() - TASK_H) {
        g_repaint = true;
        return;
    }
    window_t *w = &g_win[i];
    for (int k = i + 1; k < MAX_WIN; k++) {
        if (g_win[k].used && rect_hits((int)w->x, (int)w->y, (int)w->w, (int)w->h, &g_win[k])) {
            g_repaint = true;
            return;
        }
    }
    cursor_restore();
    draw_window(w, true);
    cursor_show();
    g_repaint = false;
}

static void browser_scroll(int delta) {
    uint32_t rows = br_layout(false, 0, 0, 0);
    uint32_t max_top = rows > BR_VISIBLE ? rows - BR_VISIBLE : 0;
    if (delta < 0 && g_br.top > 0) {
        g_br.top--;
    } else if (delta > 0 && g_br.top < max_top) {
        g_br.top++;
    } else {
        g_repaint = false;
        return;
    }
    browser_redraw();
}

/* Turn a Location header into an absolute http:// address. Only absolute addresses and
 * root-relative paths ("/x") are handled. */
static bool br_resolve(const char *base, const char *loc, char *out, uint32_t cap) {
    uint32_t n = 0;
    out[0] = '\0';
    if (strncmp(loc, "http://", 7) == 0) {
        br_append(out, cap, loc);
        return out[0] != '\0';
    }
    if (loc[0] != '/' || strncmp(base, "http://", 7) != 0) return false;
    const char *host_end = base + 7;
    while (*host_end != '\0' && *host_end != '/') host_end++;
    for (const char *p = base; p < host_end && n + 1 < cap; p++) out[n++] = *p;
    out[n] = '\0';
    br_append(out, cap, loc);
    return out[0] != '\0';
}

/* Fetch the address in the bar (http:// is added when missing), following redirects. */
static void browser_go(void) {
    char target[BR_URL_CAP];
    target[0] = '\0';
    if (strncmp(g_br.url, "http://", 7) != 0 && strncmp(g_br.url, "https://", 8) != 0) {
        br_append(target, sizeof target, "http://");
    }
    br_append(target, sizeof target, g_br.url);
    g_br.top = 0;
    g_br.text_len = 0;
    g_br.status[0] = '\0';
    br_append(g_br.status, sizeof g_br.status, "Loading ");
    br_append(g_br.status, sizeof g_br.status, target);
    browser_redraw();                       /* show "Loading" before the wait */

    for (int hop = 0;; hop++) {
        net_http_result_t res;
        if (!net_http_get(target, g_br_raw, BR_RAW_CAP, &res)) {
            g_br.status[0] = '\0';
            br_append(g_br.status, sizeof g_br.status, "Could not load: ");
            br_append(g_br.status, sizeof g_br.status, res.error);
            break;
        }
        bool redirect = res.status == 301 || res.status == 302 || res.status == 303 ||
                        res.status == 307 || res.status == 308;
        if (redirect && res.location[0] != '\0' && hop < BR_MAX_HOPS) {
            char next[BR_URL_CAP];
            if (!br_resolve(target, res.location, next, sizeof next)) {
                g_br.status[0] = '\0';
                br_append(g_br.status, sizeof g_br.status, "Redirect to an address this browser cannot follow");
                break;
            }
            target[0] = '\0';
            br_append(target, sizeof target, next);
            continue;
        }
        /* Plain text shows as it is; anything else goes through the tag stripper. */
        bool plain = strncmp(res.content_type, "text/plain", 10) == 0;
        if (plain) {
            uint32_t n = res.body_len < BR_TEXT_CAP - 1 ? res.body_len : BR_TEXT_CAP - 1;
            for (uint32_t i = 0; i < n; i++) {
                uint8_t c = res.body[i];
                g_br_text[i] = (c == '\r' || c == '\t') ? ' ' : (c >= 0x7f ? '?' : (char)c);
            }
            g_br_text[n] = '\0';
            g_br.text_len = n;
        } else {
            g_br.text_len = html_to_text(res.body, res.body_len, g_br_text, BR_TEXT_CAP);
        }
        g_br.status[0] = '\0';
        br_append(g_br.status, sizeof g_br.status, "HTTP ");
        br_append_uint(g_br.status, sizeof g_br.status, (uint32_t)res.status);
        br_append(g_br.status, sizeof g_br.status, ", ");
        br_append_uint(g_br.status, sizeof g_br.status, res.body_len);
        br_append(g_br.status, sizeof g_br.status, " bytes");
        break;
    }
    /* The bar shows the address that was finally loaded. */
    if (target[0] != '\0') {
        uint32_t n = 0;
        while (target[n] != '\0' && n + 1 < BR_URL_CAP) {
            g_br.url[n] = target[n];
            n++;
        }
        g_br.url[n] = '\0';
        g_br.url_len = n;
    }
    browser_redraw();
}

/* Keys typed into the focused browser. Esc closes the window (the desktop stays up). */
static void browser_key(int c) {
    if (c == 0x1b) {
        close_window(g_focus);
        return;
    }
    if (c == '\t') {
        focus_next();
        return;
    }
    if (c == '\n') {
        browser_go();
        return;
    }
    if (c == 0x08 || c == 0x7f) {
        if (g_br.url_len == 0) {
            g_repaint = false;
            return;
        }
        g_br.url[--g_br.url_len] = '\0';
    } else if (c >= 0x20 && c < 0x7f) {
        if (g_br.url_len + 1 >= BR_URL_CAP) {
            g_repaint = false;
            return;
        }
        g_br.url[g_br.url_len++] = (char)c;
        g_br.url[g_br.url_len] = '\0';
    } else if (c == SHELL_KEY_UP) {
        browser_scroll(-1);
        return;
    } else if (c == SHELL_KEY_DOWN) {
        browser_scroll(1);
        return;
    } else {
        g_repaint = false;
        return;
    }
    browser_redraw();
}

/* Keys typed into the focused editor. Esc closes the editor (it does not leave the
 * desktop, so a stray Esc cannot discard the window); Tab moves focus as elsewhere. */
static void editor_key(int c) {
    bool changed = false;
    if (c == 0x1b) {
        close_window(g_focus);
        return;
    }
    if (c == '\t') {
        focus_next();
        return;
    }
    if (c == '\n') {
        editor_insert(&g_ed, '\n');
        changed = true;
    } else if (c == 0x08 || c == 0x7f) {
        editor_backspace(&g_ed);
        changed = true;
    } else if (c == SHELL_KEY_DELETE) {
        editor_delete(&g_ed);
        changed = true;
    } else if (c >= 0x20 && c < 0x7f) {
        editor_insert(&g_ed, (char)c);
        changed = true;
    } else if (c == SHELL_KEY_LEFT) {
        changed = editor_move(&g_ed, ED_LEFT);
    } else if (c == SHELL_KEY_RIGHT) {
        changed = editor_move(&g_ed, ED_RIGHT);
    } else if (c == SHELL_KEY_UP) {
        changed = editor_move(&g_ed, ED_UP);
    } else if (c == SHELL_KEY_DOWN) {
        changed = editor_move(&g_ed, ED_DOWN);
    } else if (c == SHELL_KEY_HOME) {
        changed = editor_move(&g_ed, ED_HOME);
    } else if (c == SHELL_KEY_END) {
        changed = editor_move(&g_ed, ED_END);
    }
    if (!changed) {
        g_repaint = false;          /* a key the editor does not use needs no repaint */
        return;
    }
    editor_redraw();
}

/* Returns true when the desktop should close. */
static bool handle_key(int c) {
    klog_event(KLOG_INPUT, "key %d", c);
    g_repaint = true;
    if (c == SHELL_KEY_ALT_TAB) {              /* Alt+Tab: the next shown window comes forward */
        focus_next();
        return false;
    }
    if (c == SHELL_KEY_ALT_F4) {               /* Alt+F4 closes the focused window */
        close_window(g_focus);
        return false;
    }
    if (g_ctx_open) {                  /* any key closes the desktop menu; Esc only closes it */
        g_ctx_open = false;
        if (c == 0x1b) return false;
    }
    if (g_menu_open) {
        if (c == SHELL_KEY_UP) {
            menu_move(-1);
        } else if (c == SHELL_KEY_DOWN) {
            menu_move(1);
        } else if (c == SHELL_KEY_LEFT || c == SHELL_KEY_RIGHT) {
            g_menu_col = c == SHELL_KEY_LEFT ? 0 : 1;
            g_menu_sel = 0;
        } else if (c == '\n') {
            app_t app;
            if (menu_selected_app(&app)) return menu_run_item(app);
        } else if (c == 0x1b) {
            g_menu_open = false;
            g_menu_query_len = 0;
            g_menu_query[0] = '\0';
        } else if (c == 0x08 || c == 0x7f) {
            if (g_menu_query_len > 0) g_menu_query[--g_menu_query_len] = '\0';
            g_menu_sel = 0;
            g_menu_col = 0;
        } else if (c >= 0x20 && c < 0x7f && g_menu_query_len < 15) {
            g_menu_query[g_menu_query_len++] = (char)c;
            g_menu_query[g_menu_query_len] = '\0';
            g_menu_sel = 0;
            g_menu_col = 0;
        }
        return false;
    }

    if (g_focus >= 0 && g_win[g_focus].used && g_win[g_focus].app == APP_TERMINAL) {
        /* The focused window is the terminal: keys type into it. Esc still leaves the desktop. */
        if (c == 0x1b) return true;
        if (c == '\t') {
            focus_next();
        } else if (c == '\n') {
            term_submit();
        } else if (c == 0x08 || c == 0x7f) {
            int old = g_term_in_len;
            term_backspace();
            if (old != g_term_in_len) {
                term_redraw_input(old);
                g_repaint = false;
            }
        } else if (c >= 0x20 && c < 0x7f) {
            int old = g_term_in_len;
            term_key((char)c);
            if (old != g_term_in_len) {
                term_redraw_input(old);
                g_repaint = false;
            }
        }
        return false;
    }

    if (g_focus >= 0 && g_win[g_focus].used && g_win[g_focus].app == APP_EDITOR) {
        editor_key(c);
        return false;
    }

    if (g_focus >= 0 && g_win[g_focus].used && g_win[g_focus].app == APP_SETTINGS &&
        (c == 't' || c == 'T' || c == '\n')) {
        g_theme_idx = (g_theme_idx + 1u) % THEME_COUNT;   /* the keyboard way to the Appearance button */
        theme_apply();
        return false;
    }

    if (g_focus >= 0 && g_win[g_focus].used && g_win[g_focus].app == APP_NETWORK) {
        /* The host field takes typing; Enter pings. Tab and Esc keep their meaning below. */
        if (c >= 0x20 && c < 0x7f) {
            if (g_net.host_len + 1 < NET_HOST_CAP) {
                g_net.host[g_net.host_len++] = (char)c;
                g_net.host[g_net.host_len] = '\0';
            }
            return false;
        }
        if (c == 0x08 || c == 0x7f) {
            if (g_net.host_len > 0) g_net.host[--g_net.host_len] = '\0';
            return false;
        }
        if (c == '\n') {
            net_run(true);
            return false;
        }
    }

    if (g_focus >= 0 && g_win[g_focus].used && g_win[g_focus].app == APP_MYCOMP && exp_key(c)) {
        return false;                  /* Up/Down/Enter/Backspace went to the folder list */
    }

    if (g_focus >= 0 && g_win[g_focus].used && g_win[g_focus].app == APP_BROWSER) {
        browser_key(c);
        return false;
    }

    if (c == SHELL_KEY_UP) {
        g_icon_sel = (g_icon_sel + ICON_COUNT - 1) % ICON_COUNT;
    } else if (c == SHELL_KEY_DOWN) {
        g_icon_sel = (g_icon_sel + 1) % ICON_COUNT;
    } else if (c == '\n') {
        open_app(g_icon_app[g_icon_sel]);
    } else if (c == 's' || c == 'S') {
        g_menu_open = true;
        g_menu_sel = 0;
        g_menu_col = 0;
        g_menu_query_len = 0;
        g_menu_query[0] = '\0';
    } else if (c == '\t') {
        focus_next();
    } else if (c == 'x' || c == 'X') {
        close_focused();
    } else if (c == 0x1b) {
        return true;
    }
    return false;
}

/* True until someone signs in; leaving the desktop signs the user out again. */
static bool g_login_pending = true;

bool desktop_run(void) {
    if (!fb_ready()) return false;
    uint32_t sw = fb_width(), sh = fb_height();
    if (sw < 640 || sh < 480) {
        klog_note(KLOG_DESKTOP, "[desktop] screen too small (%llux%llu), need 640x480\n",
                (unsigned long long)sw, (unsigned long long)sh);
        return false;
    }

    c_white = fb_rgb(0xFF, 0xFF, 0xFF);   /* fixed: bevels, terminal text */
    c_black = fb_rgb(0x00, 0x00, 0x00);   /* fixed: terminal background, cursor */
    theme_apply();
    c_outline = fb_rgb(0xFF, 0xFF, 0xFF);     /* XOR mask for the drag outline */

    g_mx = (int)sw / 2;
    g_my = (int)sh / 2;
    g_cur_shown = false;
    g_outline_shown = false;
    g_drag = -1;
    usb_host_poll();
    usb_hid_mouse_t discard;
    usb_hid_get_mouse_event(&discard);     /* drop motion from before the desktop */
    ps2_mouse_get_event(&discard);
    g_button_was_down = (discard.buttons & 1u) != 0;

    for (int i = 0; i < MAX_WIN; i++) g_win[i].used = false;
    g_term_started = false;
    g_term_n = 0;
    g_term_in_len = 0;
    g_term_in[0] = '\0';
    g_focus = -1;
    g_icon_sel = 0;
    g_menu_sel = 0;
    g_menu_open = false;
    g_menu_col = 0;
    g_menu_query_len = 0;
    g_menu_query[0] = '\0';
    g_recent_n = 0;
    g_ctx_open = false;
    g_hover = HOVER_NONE;
    g_right_was_down = false;

    fb_graphics_begin();   /* the text console stays quiet while the desktop is up */
    if (g_login_pending) {  /* the desktop starts only after a sign-in */
        if (!login_run()) {
            fb_graphics_end();
            return false;
        }
        g_login_pending = false;
    }
    klog_note(KLOG_DESKTOP, "[desktop] started (%llux%llu)\n", (unsigned long long)sw, (unsigned long long)sh);
    redraw();

    int last_minute = -1;
    for (;;) {
        fb_present();                   /* show what the last event drew */
        int p = poll_pointer();
        if (p == PTR_EXIT) break;
        int c = input_getchar();
        if (c == KBD_SWITCH_KEY) {      /* Ctrl+Alt+T: leave for the TUI shell */
            ui_request_switch();
            break;
        }
        if (c < 0) {
            /* Idle: repaint when the pointer moved or the clock's minute changed. */
            int y, mo, d, h, mi, s, wd;
            time_now_utc(&y, &mo, &d, &h, &mi, &s, &wd);
            if (p == PTR_REDRAW) {
                last_minute = mi;
                redraw();
            } else if (mi != last_minute) {
                last_minute = mi;               /* only the clock changed: repaint the taskbar */
                cursor_restore();
                draw_taskbar(sw, sh);
                cursor_show();
            }
            __asm__ volatile("pause");
            continue;
        }
        if (handle_key(c)) break;
        if (g_repaint || p == PTR_REDRAW) redraw();
    }

    fb_graphics_end();     /* hand the screen back to the text console */
    if (!ui_switch_requested()) {
        g_login_pending = true;   /* leaving the desktop signs the user out */
    }
    klog_note(KLOG_DESKTOP, "[desktop] exited\n");
    return true;
}
