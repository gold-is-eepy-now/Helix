/* login.c — the sign-in screen. See login.h. */
#include "login.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "accounts.h"
#include "usb_host.h"
#include "fb.h"
#include "input.h"
#include "keyboard.h"
#include "kstring.h"
#include "ui.h"
#include "time.h"

#define PANEL_W   440u
#define PANEL_H   272u
#define FIELD_W   260u
#define FIELD_H   24u
#define BTN_W     104u
#define BTN_H     26u

typedef enum { FIELD_USER, FIELD_PASS } field_t;

typedef struct {
    char user[ACCT_NAME_MAX + 1];
    char pass[ACCT_PASS_MAX + 1];
    size_t ulen;
    size_t plen;
    field_t active;
    const char *message;
    bool create;          /* no accounts yet: this screen creates the first one */
    bool no_disk;         /* accounts are kept in memory only */
    bool penalty;         /* the last attempt was a wrong guess */
} login_state_t;

/* Colours, packed for the framebuffer when login_run starts. */
static uint32_t c_face, c_white, c_black, c_dark, c_light, c_title, c_text, c_error, c_hint;

static void load_colors(void) {
    c_face  = fb_rgb(0xF0, 0xF0, 0xF0);
    c_white = fb_rgb(0xFF, 0xFF, 0xFF);
    c_black = fb_rgb(0x00, 0x00, 0x00);
    c_dark  = fb_rgb(0x6E, 0x6E, 0x6E);
    c_light = fb_rgb(0xFF, 0xFF, 0xFF);
    c_title = fb_rgb(0x1F, 0x4E, 0x8C);
    c_text  = fb_rgb(0x1E, 0x1E, 0x1E);
    c_error = fb_rgb(0xB0, 0x20, 0x20);
    c_hint  = fb_rgb(0xDD, 0xE8, 0xF5);
}

/* Offset that centres `inner` in `outer`, or 0 if it does not fit. */
static uint32_t center(uint32_t outer, uint32_t inner) {
    return outer > inner ? (outer - inner) / 2u : 0u;
}

/* Clear memory in a way the compiler may not drop as a dead store. */
static void secure_zero(void *p, size_t n) {
    volatile uint8_t *b = (volatile uint8_t *)p;
    while (n--) *b++ = 0;
}

static void draw_border(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    fb_fill_rect(x, y, w, 1, color);
    fb_fill_rect(x, y + h - 1, w, 1, color);
    fb_fill_rect(x, y, 1, h, color);
    fb_fill_rect(x + w - 1, y, 1, h, color);
}

static void draw_field(uint32_t x, uint32_t y, const char *text, size_t len,
                       bool masked, bool active) {
    fb_fill_rect(x, y, FIELD_W, FIELD_H, c_white);
    draw_border(x, y, FIELD_W, FIELD_H, active ? c_title : c_dark);
    if (active) draw_border(x + 1, y + 1, FIELD_W - 2, FIELD_H - 2, c_title);

    /* Show only the last characters that fit, so long entries stay inside the box. */
    size_t room = (FIELD_W - 16u) / 8u - 1u;    /* one cell is kept for the caret */
    size_t start = len > room ? len - room : 0;
    char shown[ACCT_PASS_MAX + 2];
    size_t n = 0;
    for (size_t i = start; i < len && n < room; i++) {
        shown[n++] = masked ? '*' : text[i];
    }
    if (active) shown[n++] = '_';           /* a plain caret */
    shown[n] = '\0';
    fb_draw_text(x + 8, y + 8, shown, c_text, c_white, true);
}

static void draw_button(uint32_t x, uint32_t y, const char *label, bool primary) {
    fb_fill_rect(x, y, BTN_W, BTN_H, c_face);
    draw_border(x, y, BTN_W, BTN_H, primary ? c_title : c_dark);
    uint32_t tx = x + (BTN_W - (uint32_t)strlen(label) * 8u) / 2u;
    fb_draw_text(tx, y + 9, label, c_text, c_face, true);
}

/* Blue gradient backdrop, one 8-pixel band at a time. Drawn once: repainting it
 * under the panel on every pass is what made the screen flicker. */
static void draw_backdrop(void) {
    uint32_t sw = fb_width(), sh = fb_height();
    if (fb_draw_wallpaper(0x1B3A6Bu, 0x4F86C6u)) return;   /* cached gradient */
    for (uint32_t y = 0; y < sh; y += 8) {
        uint32_t t = y * 255u / sh;
        uint8_t r = (uint8_t)(0x1B + ((0x4F - 0x1B) * t) / 255u);
        uint8_t g = (uint8_t)(0x3A + ((0x86 - 0x3A) * t) / 255u);
        uint8_t b = (uint8_t)(0x6B + ((0xC6 - 0x6B) * t) / 255u);
        uint32_t h = sh - y < 8 ? sh - y : 8;
        fb_fill_rect(0, y, sw, h, fb_rgb(r, g, b));
    }

    const char *hint = "Tab: next field    Enter: sign in    Esc: cancel";
    uint32_t hx = center(sw, (uint32_t)strlen(hint) * 8u);
    fb_draw_text(hx, sh > 28u ? sh - 28u : 0u, hint, c_hint, 0, false);
}

/* The panel only: redrawn when the state changes. */
static void draw_panel(const login_state_t *st) {
    uint32_t sw = fb_width(), sh = fb_height();
    uint32_t px = center(sw, PANEL_W);
    uint32_t py = center(sh, PANEL_H);
    fb_fill_rect(px, py, PANEL_W, PANEL_H, c_face);
    draw_border(px, py, PANEL_W, PANEL_H, c_dark);

    /* Title strip, like a window caption. */
    fb_fill_rect(px, py, PANEL_W, 30, c_title);
    fb_draw_text(px + 12, py + 11,
                 st->create ? "Create the first account" : "Sign in to Helix OS",
                 c_light, c_title, true);

    fb_draw_text(px + 24, py + 46,
                 st->create ? "Choose a user name and password for this machine."
                            : "Enter your user name and password.",
                 c_text, c_face, true);

    fb_draw_text(px + 24, py + 88, "User name", c_text, c_face, true);
    draw_field(px + 140, py + 80, st->user, st->ulen, false, st->active == FIELD_USER);

    fb_draw_text(px + 24, py + 128, "Password", c_text, c_face, true);
    draw_field(px + 140, py + 120, st->pass, st->plen, true, st->active == FIELD_PASS);

    if (st->message && st->message[0] != '\0') {
        fb_draw_text(px + 24, py + 168, st->message, c_error, c_face, true);
    }

    if (st->no_disk) {
        fb_draw_text(px + 24, py + 196, "No disk found: accounts last until shutdown.",
                     c_dark, c_face, true);
    }

    draw_button(px + PANEL_W - BTN_W - 24u, py + PANEL_H - BTN_H - 20u,
                st->create ? "Create" : "Sign in", true);
    draw_button(px + PANEL_W - 2u * BTN_W - 36u, py + PANEL_H - BTN_H - 20u, "Cancel", false);
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

/* A status line at the foot of the screen: what USB found, and what it has received.
 * On a machine with no PS/2 port this is the way to see whether the keyboard and mouse
 * were found at all. Pressing a key or moving the mouse should raise the report counts. */
static void draw_usb_line(void) {
    usb_summary_t u;
    usb_host_summary(&u);
    uint32_t sw = fb_width(), sh = fb_height();
    uint32_t y = sh - 22u;
    fb_fill_rect(0, y, sw, 22, c_face);
    char line[160];
    size_t at = cat_str(line, sizeof line, 0, "USB: ");
    at = cat_dec(line, sizeof line, at, u.controllers);
    at = cat_str(line, sizeof line, at, " controller(s), ");
    at = cat_dec(line, sizeof line, at, u.devices);
    at = cat_str(line, sizeof line, at, " device(s).  Keyboards ");
    at = cat_dec(line, sizeof line, at, u.keyboards);
    at = cat_str(line, sizeof line, at, " (reports ");
    at = cat_dec(line, sizeof line, at, u.keyboard_reports);
    at = cat_str(line, sizeof line, at, ", errors ");
    at = cat_dec(line, sizeof line, at, u.keyboard_errors);
    at = cat_str(line, sizeof line, at, ").  Mice ");
    at = cat_dec(line, sizeof line, at, u.mice);
    at = cat_str(line, sizeof line, at, " (reports ");
    at = cat_dec(line, sizeof line, at, u.mouse_reports);
    cat_str(line, sizeof line, at, ").");
    fb_draw_text(12, y + 7, line, c_text, c_face, true);
}

static void wipe(login_state_t *st) {
    secure_zero(st->pass, sizeof st->pass);
    st->plen = 0;
}

/* Returns true when the account was accepted and the session is set. */
static bool submit(login_state_t *st) {
    st->user[st->ulen] = '\0';
    st->pass[st->plen] = '\0';

    if (st->create) {
        if (!accounts_name_valid(st->user)) {
            st->message = "User names use letters, digits, '-' and '_'.";
            return false;
        }
        if (st->plen == 0) {
            st->message = "Enter a password.";
            return false;
        }
        uint32_t uid = 0;
        if (!accounts_create(st->user, st->pass, &uid)) {
            st->message = "The account could not be created.";
            wipe(st);
            return false;
        }
        accounts_set_session(st->user, uid);
        return true;
    }

    uint32_t uid = 0;
    if (!accounts_verify(st->user, st->pass, &uid)) {
        wipe(st);
        st->message = "The user name or password is not correct.";
        st->penalty = true;     /* login_run pauses after showing the message */
        return false;
    }
    accounts_set_session(st->user, uid);
    return true;
}

/* A pause after each wrong guess slows down guessing. Uses the real clock when
 * it is calibrated; otherwise a rough spin. */
static void guess_delay(void) {
    if (time_available()) {
        uint64_t until = time_uptime_ms() + 1000u;
        while (time_uptime_ms() < until) {
            __asm__ volatile("pause");
        }
        return;
    }
    for (volatile uint32_t i = 0; i < 40000000u; i++) {
    }
}

bool login_run(void) {
    if (!fb_ready()) return false;
    load_colors();

    login_state_t st;
    memset(&st, 0, sizeof st);
    st.active = FIELD_USER;
    st.create = accounts_count() == 0;
    st.no_disk = !accounts_persistent();
    st.message = "";

    draw_backdrop();
    bool redraw = true;
    int prev = 0;
    uint64_t usb_next = 0;

    for (;;) {
        if (redraw) {
            draw_panel(&st);
            draw_usb_line();
            fb_present();
            redraw = false;
        }

        int c = input_getchar();
        if (c < 0) {
            /* Idle: refresh only the USB line, twice a second, as the counts change. */
            uint64_t now = time_uptime_ms();
            if (now >= usb_next) {
                usb_next = now + 500;
                draw_usb_line();
                fb_present();
            }
            __asm__ volatile("pause");
            continue;
        }
        if (c == '\n' && prev == '\r') {      /* the second half of a CR LF pair */
            prev = 0;
            continue;
        }
        prev = c;
        redraw = true;
        if (c == KBD_SWITCH_KEY) {           /* Ctrl+Alt+T: go to the TUI shell */
            ui_request_switch();
            secure_zero(&st, sizeof st);
            return false;
        }
        if (c == 27 || c == KBD_KEY_ESCAPE) {
            secure_zero(&st, sizeof st);
            return false;
        }

        if (c == '\t') {
            st.active = st.active == FIELD_USER ? FIELD_PASS : FIELD_USER;
            st.message = "";
            continue;
        }
        if (c == '\n' || c == '\r') {          /* serial consoles send CR for Enter */
            if (submit(&st)) {
                secure_zero(&st, sizeof st);        /* do not leave the password on the stack */
                return true;
            }
            if (st.penalty) {
                st.penalty = false;
                draw_panel(&st);                    /* show the message, then make them wait */
                fb_present();
                guess_delay();
            }
            continue;
        }
        if (c == 0x7F || c == 0x08) {            /* serial consoles often send BS, not DEL */
            st.message = "";
            if (st.active == FIELD_USER && st.ulen > 0) st.user[--st.ulen] = '\0';
            if (st.active == FIELD_PASS && st.plen > 0) st.pass[--st.plen] = '\0';
            continue;
        }
        if (c >= 0x20 && c < 0x7F) {
            st.message = "";
            if (st.active == FIELD_USER) {
                if (st.ulen < ACCT_NAME_MAX) st.user[st.ulen++] = (char)c;
            } else {
                if (st.plen < ACCT_PASS_MAX) st.pass[st.plen++] = (char)c;
            }
        }
    }
}
