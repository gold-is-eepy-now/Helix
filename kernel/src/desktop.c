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
#include "keyboard.h"
#include "klog.h"
#include "ps2_mouse.h"
#include "kstring.h"
#include "panic.h"
#include "shell.h"
#include "time.h"
#include "usb_host.h"
#include "usb_hid.h"

/* Win98-style palette, packed for the framebuffer at run time. */
static uint32_t c_desk, c_face, c_white, c_dark, c_black, c_navy, c_gray;

typedef enum { APP_MYCOMP, APP_ABOUT, APP_SHUTDOWN, APP_TERMINAL, APP_EDITOR, APP_COUNT } app_t;

static const char *const g_app_names[APP_COUNT] = { "My Computer", "About Helix", "Shut Down", "Terminal", "Text Editor" };

#define ICON_COUNT 4            /* icons on the desktop: My Computer, About Helix, Terminal, Text Editor */
#define MENU_COUNT 3            /* Start menu items, the last one is Shut Down */
#define MAX_WIN 4
#define MAX_LINES 16
#define LINE_LEN 48
#define TASK_H 28u

/* The app each desktop icon opens (icon i is not app i, since Shut Down is an app too). */
static const app_t g_icon_app[ICON_COUNT] = { APP_MYCOMP, APP_ABOUT, APP_TERMINAL, APP_EDITOR };

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

static void draw_window(const window_t *w, bool focused) {
    fb_fill_rect(w->x, w->y, w->w, w->h, c_face);
    bevel(w->x, w->y, w->w, w->h, true);

    /* Title bar: navy when focused, grey otherwise. */
    uint32_t tx = w->x + 3, ty = w->y + 3, tw = w->w - 6;
    fb_fill_rect(tx, ty, tw, 18, focused ? c_navy : c_gray);
    fb_draw_text(tx + 4, ty + 5, g_app_names[w->app], c_white, 0, false);

    /* Close button at the right end of the title bar. */
    uint32_t bx = tx + tw - 18;
    fb_fill_rect(bx, ty + 2, 16, 14, c_face);
    bevel(bx, ty + 2, 16, 14, true);
    fb_draw_text(bx + 4, ty + 5, "x", c_black, 0, false);

    /* Client area: a sunken white panel with the window's text. */
    uint32_t cx = w->x + 6, cy = w->y + 26;
    uint32_t cw = w->w - 12, ch = w->h - 32;
    fb_fill_rect(cx, cy, cw, ch, c_white);
    bevel(cx, cy, cw, ch, false);
    if (w->app == APP_TERMINAL) {
        term_draw(cx, cy, cw, ch);
        return;
    }
    if (w->app == APP_EDITOR) {
        draw_editor(cx, cy, cw, ch);
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
        fb_fill_rect(label_x - 2, y + 36, label_w, 12, c_navy);
        fb_draw_text(label_x, y + 38, name, c_white, 0, false);
    } else {
        fb_draw_text(label_x, y + 38, name, c_white, 0, false);
    }
}

static void draw_taskbar(uint32_t sw, uint32_t sh) {
    uint32_t ty = sh - TASK_H;
    fb_fill_rect(0, ty, sw, TASK_H, c_face);
    fb_fill_rect(0, ty, sw, 1, c_white);

    /* Start button: pressed in while the menu is open. */
    bevel(2, ty + 2, 60, 22, !g_menu_open);
    fb_fill_rect(3, ty + 3, 58, 20, g_menu_open ? c_gray : c_face);
    fb_draw_text(16, ty + 9, "Start", c_black, 0, false);

    /* One button per open window, pressed in for the focused one. */
    uint32_t bx = 70;
    for (int i = 0; i < MAX_WIN; i++) {
        if (!g_win[i].used) continue;
        bool focused = (i == g_focus);
        bevel(bx, ty + 2, 130, 22, !focused);
        fb_fill_rect(bx + 1, ty + 3, 128, 20, focused ? c_gray : c_face);
        char title[20];
        put_str(title, sizeof(title), g_app_names[g_win[i].app]);
        title[chars_in(120)] = '\0';
        fb_draw_text(bx + 8, ty + 9, title, c_black, 0, false);
        bx += 136;
    }

    /* Clock in a sunken box at the right. */
    int year, month, day, hour, minute, second, weekday;
    time_now_utc(&year, &month, &day, &hour, &minute, &second, &weekday);
    char clock[8];
    clock[0] = (char)('0' + hour / 10);
    clock[1] = (char)('0' + hour % 10);
    clock[2] = ':';
    clock[3] = (char)('0' + minute / 10);
    clock[4] = (char)('0' + minute % 10);
    clock[5] = '\0';
    bevel(sw - 66, ty + 3, 62, 22, false);
    fb_draw_text(sw - 56, ty + 9, clock, c_black, 0, false);
}

static void draw_start_menu(uint32_t sh) {
    uint32_t mh = MENU_COUNT * 22u + 8u;
    uint32_t my = sh - TASK_H - mh;
    fb_fill_rect(2, my, 180, mh, c_face);
    bevel(2, my, 180, mh, true);
    fb_fill_rect(4, my + 4, 22, mh - 8, c_navy);
    fb_draw_text(9, my + 8, "Helix", c_white, 0, false);
    for (int i = 0; i < MENU_COUNT; i++) {
        uint32_t iy = my + 4 + (uint32_t)i * 22u;
        bool sel = (i == g_menu_sel);
        if (sel) fb_fill_rect(28, iy, 152, 20, c_navy);
        fb_draw_text(36, iy + 6, g_app_names[i], sel ? c_white : c_black, 0, false);
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
    fb_fill_rect(0, 0, sw, sh, c_desk);
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
        g_win[i].w = app == APP_TERMINAL ? 480 : app == APP_EDITOR ? 420 : 380;
        g_win[i].h = app == APP_TERMINAL ? 330 : app == APP_EDITOR ? 280 : 250;
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

    if (inside(x, y, 2, ty + 2, 60, 22)) {                     /* Start button */
        g_menu_open = !g_menu_open;
        g_menu_sel = 0;
        return PTR_REDRAW;
    }

    uint32_t bx = 70;                                          /* taskbar window buttons */
    for (int i = 0; i < MAX_WIN; i++) {
        if (!g_win[i].used) continue;
        if (inside(x, y, bx, ty + 2, 130, 22)) {
            g_focus = i;
            return PTR_REDRAW;
        }
        bx += 136;
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
    klog_note(KLOG_DESKTOP, "[desktop] started (%llux%llu)\n", (unsigned long long)sw, (unsigned long long)sh);
    redraw();

    int last_minute = -1;
    for (;;) {
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
