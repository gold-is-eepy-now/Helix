/* console.h — the kernel's single text output channel.
 *
 * Everything the kernel prints goes through here. The console fans text out
 * to every backend that is available (serial and, if the firmware gave us
 * one, the framebuffer), and carries a current colour. On serial, colour is
 * emitted as ANSI escape sequences; on the framebuffer it is packed RGB.
 *
 * Readability rules (learned the hard way):
 *   - The framebuffer background is always dark, and the default foreground is
 *     a soft light grey. Foreground and background are packed separately, so
 *     "default" means light text on a dark background, never white-on-white.
 *   - On serial we use standard-intensity ANSI colours; "bright" palette
 *     entries become bold + standard colour. That stays legible on both light
 *     and dark terminals, where raw bright colours do not.
 *   - `console_set_color_enabled(false)` suppresses ANSI colour entirely, for
 *     terminals where even that is hard to read.
 */
#ifndef HELIX_CONSOLE_H
#define HELIX_CONSOLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct limine_framebuffer;

typedef enum {
    CONSOLE_DEFAULT = 0,
    CONSOLE_BLACK,
    CONSOLE_RED,
    CONSOLE_GREEN,
    CONSOLE_YELLOW,
    CONSOLE_BLUE,
    CONSOLE_MAGENTA,
    CONSOLE_CYAN,
    CONSOLE_WHITE,
    CONSOLE_GRAY,
    CONSOLE_BRIGHT_RED,
    CONSOLE_BRIGHT_GREEN,
    CONSOLE_BRIGHT_YELLOW,
    CONSOLE_BRIGHT_BLUE,
    CONSOLE_BRIGHT_MAGENTA,
    CONSOLE_BRIGHT_CYAN,
    CONSOLE_BRIGHT_WHITE
} console_color_t;

/* Bring up the console. `fb` may be NULL if no framebuffer is available. */
void console_init(struct limine_framebuffer *fb);

/* Write a NUL-terminated string / one character to every active backend. */
void console_write(const char *s);
void console_putc(char c);

/* Rub out the previous character on every backend. */
void console_backspace(void);

/* Clear the screen (and home the cursor). */
void console_clear(void);
void console_cursor_to(size_t row, size_t column);
void console_clear_current_line(void);

/* Colour control. Colours apply to subsequent writes. */
void console_set_fg(console_color_t color);
void console_set_bg(console_color_t color);
void console_reset_color(void);

/* Turn ANSI colour output on the serial backend on or off. When off, the
 * terminal's own default colours are used, which is the safest choice on a
 * light-background terminal. */
void console_set_color_enabled(bool enabled);
bool console_color_enabled(void);

/* Convenience helpers for diagnostics. */
void console_write_dec(uint64_t value);
void console_write_hex(uint64_t value);

#endif /* HELIX_CONSOLE_H */
