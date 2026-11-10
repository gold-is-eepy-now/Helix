/* desktop.c — a basic Windows 98-style desktop, drawn on the framebuffer.
 *
 * What it is: a teal desktop with two icons, a grey taskbar with a Start menu
 * and a clock, and bevelled windows with navy title bars. "My Computer" lists the
 * real root directory from the ext4 volume; "About Helix" is static text.
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
#include "input.h"
#include "login.h"
#include "keyboard.h"
#include "klog.h"
#include "html_text.h"
#include "net_http.h"
#include "ps2_mouse.h"
#include "kstring.h"
#include "panic.h"
#include "shell.h"
#include "time.h"
#include "usb_host.h"
#include "usb_hid.h"

/* Win98-style palette, packed for the framebuffer at run time. */
static uint32_t c_desk, c_face, c_white, c_dark, c_black, c_navy, c_gray;

typedef enum { APP_MYCOMP, APP_ABOUT, APP_SHUTDOWN, APP_TERMINAL, APP_EDITOR, APP_BROWSER, APP_COUNT } app_t;

static const char *const g_app_names[APP_COUNT] = { "My Computer", "About Helix", "Shut Down", "Terminal", "Text Editor", "Browser" };

#define ICON_COUNT 5            /* icons on the desktop: My Computer, About Helix, Terminal, Text Editor, Browser */
#define MENU_COUNT 3            /* Start menu items, the last one is Shut Down */
#define MAX_WIN 4
#define MAX_LINES 16
#define LINE_LEN 48
#define TASK_H 36u
#define TB_BTN_X    58u         /* taskbar window buttons: first x, size, step between them */
#define TB_BTN_Y    5u
#define TB_BTN_H    26u
#define TB_BTN_W    150u
#define TB_BTN_STEP 156u

/* The app each desktop icon opens (icon i is not app i, since Shut Down is an app too). */
static const app_t g_icon_app[ICON_COUNT] = { APP_MYCOMP, APP_ABOUT, APP_TERMINAL, APP_EDITOR, APP_BROWSER };

typedef struct {
    bool used;
    app_t app;
    uint32_t x, y, w, h;
} window_t;

static window_t g_win[MAX_WIN];
static editor_t g_ed;          /* the text the Text Editor window shows; one editor, one window */
static int g_focus = -1;
static int g_icon_sel;
static int g_menu_sel;
static bool g_menu_open;

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

/* Callback for ext4_readdir: one line per entry in the root directory. */
static bool list_entry(void *ctx, const ext4_stat_t *st, const char *name, uint32_t name_len) {
    (void)ctx;
    if (name_len == 1 && name[0] == '.') return false;
    if (name_len == 2 && name[0] == '.' && name[1] == '.') return false;
    if (g_nlines[APP_MYCOMP] >= MAX_LINES) return true;   /* window is full */

    char line[LINE_LEN];
    size_t at = 0;
    line[0] = '\0';
    uint32_t shown = name_len < 26 ? name_len : 26;
    for (uint32_t i = 0; i < shown; i++) {
        char c = name[i];
        if (c < 0x20 || c > 0x7e) c = '?';
        if (at + 1 < sizeof(line)) line[at++] = c;
    }
    line[at] = '\0';
    while (at < 30) {                       /* pad the name into a column */
        at = cat_str(line, sizeof(line), at, " ");
    }
    if (EXT4_S_ISDIR(st->mode)) {
        at = cat_str(line, sizeof(line), at, "<DIR>");
    } else {
        at = cat_dec(line, sizeof(line), at, st->size);
        at = cat_str(line, sizeof(line), at, " bytes");
    }
    add_line(APP_MYCOMP, line);
    return false;
}

static void build_content(app_t app) {
    g_nlines[app] = 0;
    if (app == APP_ABOUT) {
        add_line(app, "Helix OS desktop (basic test build).");
        add_line(app, "");
        add_line(app, "A Windows 98-style desktop drawn on the");
        add_line(app, "framebuffer by the Helix kernel.");
        add_line(app, "");
        add_line(app, "Tab switches windows, x closes one.");
        add_line(app, "Start > Shut Down, or Esc, leaves the desktop.");
    } else if (app == APP_MYCOMP) {
        add_line(app, "Root directory of the ext4 volume:");
        add_line(app, "");
        ext4_fs_t *fs = ext4_get_current();
        if (fs == NULL) {
            add_line(app, "(no ext4 volume is mounted)");
            return;
        }
        ext4_readdir(fs, ext4_root(fs), list_entry, NULL);
        if (g_nlines[app] == 2) {
            add_line(app, "(empty)");
        }
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

/* Paint the editor's client area: the visible lines, the caret, the status strip. */
static void draw_editor(uint32_t cx, uint32_t cy, uint32_t cw, uint32_t ch) {
    uint32_t rows = ed_rows(ch);
    editor_scroll_to_cursor(&g_ed, rows);
    uint32_t cols = (cw - 8u) / 8u;
    if (cols > 96u) cols = 96u;
    uint32_t cur_line = editor_line_of(&g_ed, g_ed.pos);
    fb_fill_rect(cx, cy, cw, ch - ED_STRIP_H, c_white);

    uint32_t off = editor_offset_of_line(&g_ed, g_ed.top);
    for (uint32_t r = 0; r < rows && off <= g_ed.len; r++) {
        uint32_t line = g_ed.top + r;
        uint32_t end = editor_line_end(&g_ed, off);
        char row[97];
        uint32_t n = 0;
        for (uint32_t i = off; i < end && n < cols; i++) row[n++] = g_ed.text[i];
        row[n] = '\0';
        uint32_t y = cy + 4u + r * 12u;
        fb_draw_text(cx + 4, y, row, c_black, 0, false);
        if (line == cur_line) {
            uint32_t px = cx + 4u + (g_ed.pos - off) * 8u;
            if (px < cx + cw - 4u) fb_fill_rect(px, y, 1, 10, c_black);   /* the caret */
        }
        if (end >= g_ed.len) break;
        off = end + 1;
    }

    uint32_t sy = cy + ch - ED_STRIP_H;
    fb_fill_rect(cx, sy, cw, ED_STRIP_H, c_face);
    fb_draw_text(cx + 4, sy + 7, EDITOR_NAME, c_black, 0, false);
    fb_draw_text(cx + 4 + (uint32_t)strlen(EDITOR_NAME) * 8u + 16u, sy + 7, g_ed.msg, c_black, 0, false);
    uint32_t bx = cx + cw - ED_SAVE_W - 4u, by = sy + 3u;
    fb_fill_rect(bx, by, ED_SAVE_W, 16, c_face);
    bevel(bx, by, ED_SAVE_W, 16, true);
    fb_draw_text(bx + 12, by + 4, "Save", c_black, 0, false);
}

/* ---- Browser window ----
 * An address bar, a status line, and the page as plain text. Enter fetches the address
 * with net_http_get and shows the body as text (html_to_text strips the tags). The fetch
 * blocks the desktop until the response arrives or times out. Only plain http:// works. */
#define BR_URL_CAP   240
#define BR_RAW_CAP   (256u * 1024u)   /* the response, then its body in place */
#define BR_TEXT_CAP  (64u * 1024u)    /* the page as plain text */
#define BR_COLS      54               /* characters per page row */
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
                fb_draw_text(px, py + (r - g_br.top) * 12u, row, c_black, 0, false);
            }
            r++;
            k += adv;
        } while (k < len);
        if (end >= n) break;
        start = end + 1;
    }
    return r;
}

static void browser_draw(uint32_t cx, uint32_t cy, uint32_t cw, uint32_t ch) {
    fb_fill_rect(cx, cy, cw, ch, c_white);

    /* Address bar: a sunken box holding the end of the address, with a caret. */
    uint32_t bx = cx + 4u, by = cy + 4u, bw = cw - 8u;
    fb_fill_rect(bx, by, bw, BR_BAR_H, c_white);
    bevel(bx, by, bw, BR_BAR_H, false);
    uint32_t fits = (bw - 8u) / 8u;
    uint32_t from = g_br.url_len > fits ? g_br.url_len - fits : 0;
    char shown[BR_URL_CAP];
    uint32_t k = 0;
    for (uint32_t i = from; i < g_br.url_len; i++) shown[k++] = g_br.url[i];
    shown[k] = '\0';
    fb_draw_text(bx + 4u, by + 5u, shown, c_black, 0, false);
    fb_fill_rect(bx + 4u + (g_br.url_len - from) * 8u, by + 3u, 1, 12, c_black);   /* caret */

    /* Page: plain text rows below the address bar. */
    uint32_t py = cy + BR_BAR_H + 12u;
    br_layout(true, cx + 6u, py, BR_VISIBLE);

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
    fb_draw_text(cx + 4u, sy + 6u, st, c_black, 0, false);
}

static void draw_window(const window_t *w, bool focused) {
    /* Glass frame with a darker outline. */
    fill_box(w->x, w->y, w->w, w->h, 6, 0x1E4C86u, 0x1E4C86u, false);
    if (focused) {
        fill_box(w->x + 1, w->y + 1, w->w - 2, w->h - 2, 5, 0xC4DDF5u, 0x7FA9D6u, false);
    } else {
        fill_box(w->x + 1, w->y + 1, w->w - 2, w->h - 2, 5, 0xE4ECF5u, 0xB6C9DEu, false);
    }

    /* Caption: the title on the glass, and a red close button. */
    uint32_t tx = w->x + 3, ty = w->y + 3, tw = w->w - 6;
    fb_draw_text(tx + 8, ty + 5, g_app_names[w->app],
                 focused ? fb_rgb(0x10, 0x23, 0x3D) : fb_rgb(0x4D, 0x64, 0x80), 0, false);

    uint32_t bx = tx + tw - 18;
    fill_box(bx, ty + 2, 16, 14, 3, 0xE8A38Cu, 0xC2472Cu, true);
    fb_draw_text(bx + 4, ty + 5, "x", fb_rgb(0xFF, 0xFF, 0xFF), 0, false);

    /* Client area: white with a thin blue-grey edge. */
    uint32_t cx = w->x + 6, cy = w->y + 26;
    uint32_t cw = w->w - 12, ch = w->h - 32;
    uint32_t edge = fb_rgb(0x5A, 0x7A, 0xA3);
    fb_fill_rect(cx, cy, cw, ch, c_white);
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
    uint32_t cols = chars_in(cw - 8);
    for (int i = 0; i < g_nlines[w->app]; i++) {
        uint32_t row_y = cy + 6 + (uint32_t)i * 12u;
        if (row_y + 8 > cy + ch) break;
        char line[LINE_LEN];
        put_str(line, sizeof(line), g_text[w->app][i]);
        if (cols < sizeof(line)) line[cols] = '\0';
        fb_draw_text(cx + 4, row_y, line, c_black, 0, false);
    }
}

static void draw_icon(int idx, uint32_t x, uint32_t y, bool selected) {
    /* A 32x32 picture per icon: a monitor, a page, a terminal. idx is the icon, see g_icon_app. */
    if (idx == 0) {
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

static void draw_taskbar(uint32_t sw, uint32_t sh) {
    uint32_t ty = sh - TASK_H;
    fill_box(0, ty, sw, TASK_H, 0, 0x33507Au, 0x0F1E36u, false);   /* dark glass */
    fb_fill_rect(0, ty, sw, 1, fb_rgb(0x8F, 0xA9, 0xCC));

    draw_orb(24, ty + 18, 14, g_menu_open);

    /* One rounded button per open window, lit for the focused one. */
    uint32_t bx = TB_BTN_X;
    for (int i = 0; i < MAX_WIN; i++) {
        if (!g_win[i].used) continue;
        bool focused = (i == g_focus);
        if (focused) {
            fill_box(bx, ty + TB_BTN_Y, TB_BTN_W, TB_BTN_H, 4, 0xA7CCF0u, 0x5B8CC6u, true);
        } else {
            fill_box(bx, ty + TB_BTN_Y, TB_BTN_W, TB_BTN_H, 4, 0x4D6E9Cu, 0x2C4870u, true);
        }
        char title[20];
        put_str(title, sizeof(title), g_app_names[g_win[i].app]);
        title[chars_in(TB_BTN_W - 20u)] = '\0';
        fb_draw_text(bx + 10, ty + TB_BTN_Y + 9, title,
                     focused ? fb_rgb(0x10, 0x23, 0x3D) : fb_rgb(0xFF, 0xFF, 0xFF), 0, false);
        bx += TB_BTN_STEP;
    }

    /* The clock, shadowed on the glass at the right. */
    int year, month, day, hour, minute, second, weekday;
    time_now_utc(&year, &month, &day, &hour, &minute, &second, &weekday);
    char clock[8];
    clock[0] = (char)('0' + hour / 10);
    clock[1] = (char)('0' + hour % 10);
    clock[2] = ':';
    clock[3] = (char)('0' + minute / 10);
    clock[4] = (char)('0' + minute % 10);
    clock[5] = '\0';
    draw_text_shadow(sw - 60, ty + 14, clock, fb_rgb(0xFF, 0xFF, 0xFF), fb_rgb(0x0B, 0x1F, 0x3A));
}

static void draw_start_menu(uint32_t sh) {
    uint32_t mh = MENU_COUNT * 22u + 8u;
    uint32_t my = sh - TASK_H - mh;
    fill_box(2, my, 180, mh, 6, 0x2B4C7Eu, 0x2B4C7Eu, false);          /* outline */
    fill_box(3, my + 1, 178, mh - 2, 5, 0xF4F8FDu, 0xDCE7F4u, false);  /* light panel */
    fill_box(4, my + 4, 22, mh - 8, 0, 0x2B4C7Eu, 0x1B3157u, false);   /* dark strip */
    fb_draw_text(9, my + 8, "Helix", fb_rgb(0xFF, 0xFF, 0xFF), 0, false);
    for (int i = 0; i < MENU_COUNT; i++) {
        uint32_t iy = my + 4 + (uint32_t)i * 22u;
        bool sel = (i == g_menu_sel);
        if (sel) fill_box(28, iy, 152, 20, 3, 0xA7CCF0u, 0x6FA2DAu, true);
        fb_draw_text(36, iy + 6, g_app_names[i],
                     sel ? fb_rgb(0x10, 0x23, 0x3D) : c_black, 0, false);
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

static void redraw(void) {
    uint32_t sw = fb_width(), sh = fb_height();
    g_cur_shown = false;                        /* the frame below wipes the old arrow */
    g_outline_shown = false;
    fill_box(0, 0, sw, sh, 0, 0x6FB6F0u, 0x0D3D78u, false);   /* sky-blue wallpaper */
    for (int i = 0; i < ICON_COUNT; i++) {
        draw_icon(i, 16, 16u + (uint32_t)i * 72u, i == g_icon_sel && !g_menu_open);
    }
    for (int i = 0; i < MAX_WIN; i++) {
        if (g_win[i].used) draw_window(&g_win[i], i == g_focus);
    }
    if (g_drag >= 0 && g_win[g_drag].used) {   /* a drag in progress: keep its outline */
        outline_xor(g_out_x, g_out_y, g_win[g_drag].w, g_win[g_drag].h);
        g_outline_shown = true;
    }
    draw_taskbar(sw, sh);
    if (g_menu_open) draw_start_menu(sh);
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

    bool covered = g_menu_open || g_drag >= 0;
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
        if (g_win[i].used) {
            g_focus = i;
            return;
        }
    }
}

static void open_app(app_t app) {
    for (int i = 0; i < MAX_WIN; i++) {
        if (g_win[i].used && g_win[i].app == app) {
            g_focus = i;               /* already open: bring it forward */
            return;
        }
    }
    for (int i = 0; i < MAX_WIN; i++) {
        if (g_win[i].used) continue;
        uint32_t n = (uint32_t)i;
        g_win[i].used = true;
        g_win[i].app = app;
        g_win[i].x = 120u + n * 28u;
        g_win[i].y = 40u + n * 28u;
        g_win[i].w = app == APP_TERMINAL ? 480 : app == APP_EDITOR ? 420 : app == APP_BROWSER ? 460 : 380;
        g_win[i].h = app == APP_TERMINAL ? 330 : app == APP_EDITOR ? 280 : app == APP_BROWSER ? 340 : 250;
        g_focus = i;
        build_content(app);
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
        if (g_win[k].used) {
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

static int handle_click(int x, int y, uint32_t sh) {
    uint32_t ty = sh - TASK_H;

    if (g_menu_open) {
        uint32_t mh = MENU_COUNT * 22u + 8u;
        uint32_t my = sh - TASK_H - mh;
        for (int i = 0; i < MENU_COUNT; i++) {
            uint32_t iy = my + 4 + (uint32_t)i * 22u;
            if (inside(x, y, 28, iy, 152, 20)) {
                g_menu_open = false;
                if (i == APP_SHUTDOWN) return PTR_EXIT;
                open_app((app_t)i);
                return PTR_REDRAW;
            }
        }
        if (inside(x, y, 2, my, 180, mh)) return PTR_REDRAW;   /* a click on the panel itself */
        g_menu_open = false;                                   /* any other click closes it */
    }

    if (inside(x, y, 4, ty + 3, 40, 30)) {                     /* Start orb */
        g_menu_open = !g_menu_open;
        g_menu_sel = 0;
        return PTR_REDRAW;
    }

    uint32_t bx = TB_BTN_X;                                    /* taskbar window buttons */
    for (int i = 0; i < MAX_WIN; i++) {
        if (!g_win[i].used) continue;
        if (inside(x, y, bx, ty + TB_BTN_Y, TB_BTN_W, TB_BTN_H)) {
            g_focus = i;
            return PTR_REDRAW;
        }
        bx += TB_BTN_STEP;
    }

    /* Windows: the close box first, then the body (which brings the window forward). */
    for (int i = MAX_WIN - 1; i >= 0; i--) {
        if (!g_win[i].used) continue;
        window_t *w = &g_win[i];
        if (inside(x, y, w->x + w->w - 21, w->y + 5, 16, 14)) {
            close_window(i);
            return PTR_REDRAW;
        }
        if (inside(x, y, w->x, w->y, w->w, 24)) {          /* title bar: start a drag */
            g_focus = i;
            g_drag = i;
            g_drag_dx = x - (int)w->x;
            g_drag_dy = y - (int)w->y;
            g_out_x = (int)w->x;
            g_out_y = (int)w->y;
            klog_event(KLOG_DESKTOP, "drag start %s", g_app_names[w->app]);
            return PTR_REDRAW;
        }
        if (inside(x, y, w->x, w->y, w->w, w->h)) {
            g_focus = i;
            if (w->app == APP_EDITOR) editor_click(w, x, y);
            return PTR_REDRAW;
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
    return PTR_NONE;
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
        } else {
            cursor_move(nx, ny);               /* plain move: only the arrow changes */
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
    if (down && !g_button_was_down) {
        klog_event(KLOG_INPUT, "click at %d,%d", g_mx, g_my);
        result = handle_click(g_mx, g_my, sh);
        if (result == PTR_NONE) result = PTR_REDRAW;   /* a click always updates the picture */
    }
    g_button_was_down = down;
    return result;
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
    draw_window(w, true);
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
    if (g_menu_open) {
        if (c == SHELL_KEY_UP) {
            g_menu_sel = (g_menu_sel + MENU_COUNT - 1) % MENU_COUNT;
        } else if (c == SHELL_KEY_DOWN) {
            g_menu_sel = (g_menu_sel + 1) % MENU_COUNT;
        } else if (c == '\n') {
            g_menu_open = false;
            if (g_menu_sel == APP_SHUTDOWN) return true;
            open_app((app_t)g_menu_sel);
        } else if (c == 0x1b || c == 's' || c == 'S') {
            g_menu_open = false;
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
    } else if (c == '\t') {
        focus_next();
    } else if (c == 'x' || c == 'X') {
        close_focused();
    } else if (c == 0x1b) {
        return true;
    }
    return false;
}

bool desktop_run(void) {
    if (!fb_ready()) return false;
    uint32_t sw = fb_width(), sh = fb_height();
    if (sw < 640 || sh < 480) {
        klog_note(KLOG_DESKTOP, "[desktop] screen too small (%llux%llu), need 640x480\n",
                (unsigned long long)sw, (unsigned long long)sh);
        return false;
    }

    c_desk  = fb_rgb(0x00, 0x80, 0x80);
    c_face  = fb_rgb(0xC0, 0xC0, 0xC0);
    c_white = fb_rgb(0xFF, 0xFF, 0xFF);
    c_dark  = fb_rgb(0x80, 0x80, 0x80);
    c_black = fb_rgb(0x00, 0x00, 0x00);
    c_navy  = fb_rgb(0x00, 0x00, 0x80);
    c_gray  = fb_rgb(0x80, 0x80, 0x80);
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

    fb_graphics_begin();   /* the text console stays quiet while the desktop is up */
    if (!login_run()) {    /* the desktop starts only after a sign-in */
        fb_graphics_end();
        return false;
    }
    klog_note(KLOG_DESKTOP, "[desktop] started (%llux%llu)\n", (unsigned long long)sw, (unsigned long long)sh);
    redraw();

    int last_minute = -1;
    for (;;) {
        fb_present();                   /* show what the last event drew */
        int p = poll_pointer();
        if (p == PTR_EXIT) break;
        int c = input_getchar();
        if (c < 0) {
            /* Idle: repaint when the pointer moved or the clock's minute changed. */
            int y, mo, d, h, mi, s, wd;
            time_now_utc(&y, &mo, &d, &h, &mi, &s, &wd);
            if (p == PTR_REDRAW || mi != last_minute) {
                last_minute = mi;
                redraw();
            }
            __asm__ volatile("pause");
            continue;
        }
        if (handle_key(c)) break;
        if (g_repaint || p == PTR_REDRAW) redraw();
    }

    fb_graphics_end();     /* hand the screen back to the text console */
    klog_note(KLOG_DESKTOP, "[desktop] exited\n");
    return true;
}
