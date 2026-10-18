/* console.c — fan text out to serial and (optionally) the framebuffer.
 *
 * The serial console is mandatory: it is how automated tests read what the
 * kernel printed. The framebuffer is best-effort. Keeping those two concerns
 * behind one small interface means the rest of the kernel never has to know
 * where text ends up — or that colour is ANSI on one backend and RGB on the
 * other.
 *
 * Foreground and background are packed *separately*. In particular, the
 * "default" background is black, not the default foreground colour. Treating
 * them as one value is how you end up painting white text on a white
 * background the moment any colour is set.
 */
#include "console.h"

#include <stdbool.h>
#include <stddef.h>

#include "fb.h"
#include "serial.h"

static bool g_fb_ok;
static bool g_color_enabled = true;
static console_color_t g_fg = CONSOLE_DEFAULT;
static console_color_t g_bg = CONSOLE_DEFAULT;

/* ------------------------------------------------------------------ */
/* Serial (ANSI) colour                                                */
/* ------------------------------------------------------------------ */

/* Standard-intensity SGR foreground codes. Bright palette entries reuse the
 * same code and add bold (see ansi_fg_is_bold), because raw bright colours
 * are unreadable on a light terminal background. */
static int ansi_fg_code(console_color_t c) {
    switch (c) {
        case CONSOLE_BLACK:          return 30;
        case CONSOLE_RED:            return 31;
        case CONSOLE_GREEN:          return 32;
        case CONSOLE_YELLOW:         return 33;
        case CONSOLE_BLUE:           return 34;
        case CONSOLE_MAGENTA:        return 35;
        case CONSOLE_CYAN:           return 36;
        case CONSOLE_WHITE:          return 37;
        case CONSOLE_GRAY:           return 90; /* bright black: dim on both */
        case CONSOLE_BRIGHT_RED:     return 31;
        case CONSOLE_BRIGHT_GREEN:   return 32;
        case CONSOLE_BRIGHT_YELLOW:  return 33;
        case CONSOLE_BRIGHT_BLUE:    return 34;
        case CONSOLE_BRIGHT_MAGENTA: return 35;
        case CONSOLE_BRIGHT_CYAN:    return 36;
        case CONSOLE_BRIGHT_WHITE:   return 37;
        default:                     return 39;
    }
}

static bool ansi_fg_is_bold(console_color_t c) {
    return c >= CONSOLE_BRIGHT_RED && c <= CONSOLE_BRIGHT_WHITE;
}

static int ansi_bg_code(console_color_t c) {
    if (c == CONSOLE_DEFAULT) {
        return 49;
    }
    return ansi_fg_code(c) + 10;
}

static void serial_write_uint(uint32_t value) {
    char buf[11];
    size_t i = sizeof(buf) - 1;
    buf[i] = '\0';
    if (value == 0) {
        buf[--i] = '0';
    } else {
        while (value > 0) {
            buf[--i] = (char)('0' + (value % 10));
            value /= 10;
        }
    }
    serial_write(&buf[i]);
}

/* Emit the current colours to the serial terminal as one SGR sequence. */
static void serial_apply_color(void) {
    if (!g_color_enabled) {
        /* Make sure nothing is left coloured from before. */
        serial_write("\x1b[0m");
        return;
    }
    serial_write("\x1b[");
    if (ansi_fg_is_bold(g_fg)) {
        serial_write("1;");
    }
    serial_write_uint((uint32_t)ansi_fg_code(g_fg));
    serial_write(";");
    serial_write_uint((uint32_t)ansi_bg_code(g_bg));
    serial_write("m");
}

/* ------------------------------------------------------------------ */
/* Framebuffer colour                                                  */
/* ------------------------------------------------------------------ */

/* A soft, high-contrast palette tuned for a dark background (One Dark-ish).
 * These are deliberately muted rather than fully saturated so text stays
 * comfortable to read for long stretches. */
static uint32_t fb_pack(console_color_t c, bool is_background) {
    if (c == CONSOLE_DEFAULT) {
        /* Default background is black; default foreground is soft white.
         * They must never be the same value. */
        return is_background ? fb_rgb(0x00, 0x00, 0x00)
                             : fb_rgb(0xDC, 0xDF, 0xE4);
    }
    switch (c) {
        case CONSOLE_BLACK:          return fb_rgb(0x28, 0x2C, 0x34);
        case CONSOLE_RED:            return fb_rgb(0xE0, 0x6C, 0x75);
        case CONSOLE_GREEN:          return fb_rgb(0x98, 0xC3, 0x79);
        case CONSOLE_YELLOW:         return fb_rgb(0xE5, 0xC0, 0x7B);
        case CONSOLE_BLUE:           return fb_rgb(0x61, 0xAF, 0xEF);
        case CONSOLE_MAGENTA:        return fb_rgb(0xC6, 0x78, 0xDD);
        case CONSOLE_CYAN:           return fb_rgb(0x56, 0xB6, 0xC2);
        case CONSOLE_WHITE:          return fb_rgb(0xDC, 0xDF, 0xE4);
        case CONSOLE_GRAY:           return fb_rgb(0x8A, 0x92, 0xA0);
        case CONSOLE_BRIGHT_RED:     return fb_rgb(0xFF, 0x8B, 0x95);
        case CONSOLE_BRIGHT_GREEN:   return fb_rgb(0xB6, 0xE3, 0x8F);
        case CONSOLE_BRIGHT_YELLOW:  return fb_rgb(0xF0, 0xD0, 0x8A);
        case CONSOLE_BRIGHT_BLUE:    return fb_rgb(0x82, 0xC4, 0xFF);
        case CONSOLE_BRIGHT_MAGENTA: return fb_rgb(0xE0, 0x95, 0xF0);
        case CONSOLE_BRIGHT_CYAN:    return fb_rgb(0x7F, 0xD6, 0xE0);
        case CONSOLE_BRIGHT_WHITE:   return fb_rgb(0xFF, 0xFF, 0xFF);
        default:                     return fb_rgb(0xDC, 0xDF, 0xE4);
    }
}

static void apply_colors(void) {
    serial_apply_color();
    if (g_fb_ok) {
        fb_set_colors(fb_pack(g_fg, false), fb_pack(g_bg, true));
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void console_init(struct limine_framebuffer *fb) {
    serial_init();
    if (fb != NULL) {
        fb_init(fb);
        g_fb_ok = fb_ready();
    } else {
        g_fb_ok = false;
    }
    g_fg = CONSOLE_DEFAULT;
    g_bg = CONSOLE_DEFAULT;
    apply_colors();
}

void console_write(const char *s) {
    serial_write(s);
    if (g_fb_ok) {
        fb_write(s);
    }
}

void console_putc(char c) {
    serial_putc(c);
    if (g_fb_ok) {
        fb_putchar(c);
    }
}

void console_backspace(void) {
    serial_write("\b \b");
    if (g_fb_ok) {
        fb_backspace();
    }
}

void console_clear(void) {
    serial_write("\x1b[2J\x1b[H");
    if (g_fb_ok) {
        fb_clear(fb_pack(g_bg, true));
    }
}

void console_cursor_to(size_t row, size_t column) {
    serial_write("\x1b[");
    serial_write_uint((uint32_t)(row + 1));
    serial_write(";");
    serial_write_uint((uint32_t)(column + 1));
    serial_write("H");
    if (g_fb_ok) fb_cursor_to(row, column);
}

void console_clear_current_line(void) {
    serial_write("\x1b[2K");
    if (g_fb_ok) fb_clear_current_line();
}

void console_set_fg(console_color_t color) {
    g_fg = color;
    apply_colors();
}

void console_set_bg(console_color_t color) {
    g_bg = color;
    apply_colors();
}

void console_reset_color(void) {
    g_fg = CONSOLE_DEFAULT;
    g_bg = CONSOLE_DEFAULT;
    apply_colors();
}

void console_set_color_enabled(bool enabled) {
    g_color_enabled = enabled;
    apply_colors();
}

bool console_color_enabled(void) {
    return g_color_enabled;
}

void console_write_dec(uint64_t value) {
    char buf[21];
    size_t i = sizeof(buf) - 1;
    buf[i] = '\0';
    if (value == 0) {
        buf[--i] = '0';
    } else {
        while (value > 0) {
            buf[--i] = (char)('0' + (value % 10));
            value /= 10;
        }
    }
    console_write(&buf[i]);
}

void console_write_hex(uint64_t value) {
    static const char digits[] = "0123456789abcdef";
    char buf[17];
    buf[16] = '\0';
    for (size_t i = 0; i < 16; i++) {
        buf[15 - i] = digits[value & 0xF];
        value >>= 4;
    }
    console_write("0x");
    console_write(buf);
}
