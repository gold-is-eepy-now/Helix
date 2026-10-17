/* console.c — fan text out to serial and (optionally) the framebuffer.
 *
 * The serial console is mandatory: it is how automated tests read what the
 * kernel printed. The framebuffer is best-effort. Keeping those two concerns
 * behind one small interface means the rest of the kernel never has to know
 * where text ends up — or that colour is ANSI on one backend and RGB on the
 * other.
 */
#include "console.h"

#include <stdbool.h>
#include <stddef.h>

#include "fb.h"
#include "serial.h"

static bool g_fb_ok;
static console_color_t g_fg = CONSOLE_DEFAULT;
static console_color_t g_bg = CONSOLE_DEFAULT;

/* ANSI SGR foreground/background codes for each palette entry. */
static int ansi_fg_code(console_color_t c) {
    switch (c) {
        case CONSOLE_BLACK:   return 30;
        case CONSOLE_RED:     return 31;
        case CONSOLE_GREEN:   return 32;
        case CONSOLE_YELLOW:  return 33;
        case CONSOLE_BLUE:    return 34;
        case CONSOLE_MAGENTA: return 35;
        case CONSOLE_CYAN:    return 36;
        case CONSOLE_WHITE:   return 37;
        case CONSOLE_GRAY:    return 90;
        case CONSOLE_BRIGHT_RED:     return 91;
        case CONSOLE_BRIGHT_GREEN:   return 92;
        case CONSOLE_BRIGHT_YELLOW:  return 93;
        case CONSOLE_BRIGHT_BLUE:    return 94;
        case CONSOLE_BRIGHT_MAGENTA: return 95;
        case CONSOLE_BRIGHT_CYAN:    return 96;
        case CONSOLE_BRIGHT_WHITE:   return 97;
        default:              return 39;
    }
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
    serial_write("\x1b[");
    serial_write_uint((uint32_t)ansi_fg_code(g_fg));
    serial_write(";");
    serial_write_uint((uint32_t)ansi_bg_code(g_bg));
    serial_write("m");
}

/* Pack a palette entry into the framebuffer's pixel format. */
static uint32_t fb_pack(console_color_t c) {
    switch (c) {
        case CONSOLE_BLACK:   return fb_rgb(0x00, 0x00, 0x00);
        case CONSOLE_RED:     return fb_rgb(0xC0, 0x30, 0x30);
        case CONSOLE_GREEN:   return fb_rgb(0x30, 0xC0, 0x30);
        case CONSOLE_YELLOW:  return fb_rgb(0xC0, 0xC0, 0x30);
        case CONSOLE_BLUE:    return fb_rgb(0x30, 0x50, 0xC0);
        case CONSOLE_MAGENTA: return fb_rgb(0xC0, 0x30, 0xC0);
        case CONSOLE_CYAN:    return fb_rgb(0x30, 0xC0, 0xC0);
        case CONSOLE_WHITE:   return fb_rgb(0xC0, 0xC0, 0xC0);
        case CONSOLE_GRAY:    return fb_rgb(0x80, 0x80, 0x80);
        case CONSOLE_BRIGHT_RED:     return fb_rgb(0xFF, 0x60, 0x60);
        case CONSOLE_BRIGHT_GREEN:   return fb_rgb(0x60, 0xFF, 0x60);
        case CONSOLE_BRIGHT_YELLOW:  return fb_rgb(0xFF, 0xFF, 0x60);
        case CONSOLE_BRIGHT_BLUE:    return fb_rgb(0x60, 0x80, 0xFF);
        case CONSOLE_BRIGHT_MAGENTA: return fb_rgb(0xFF, 0x60, 0xFF);
        case CONSOLE_BRIGHT_CYAN:    return fb_rgb(0x60, 0xFF, 0xFF);
        case CONSOLE_BRIGHT_WHITE:   return fb_rgb(0xFF, 0xFF, 0xFF);
        default:              return fb_rgb(0xE0, 0xE0, 0xE0);
    }
}

static void apply_colors(void) {
    serial_apply_color();
    if (g_fb_ok) {
        fb_set_colors(fb_pack(g_fg), fb_pack(g_bg));
    }
}

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
        fb_clear(fb_pack(g_bg == CONSOLE_DEFAULT ? CONSOLE_BLACK : g_bg));
    }
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
