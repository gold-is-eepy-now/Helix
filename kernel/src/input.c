/* input.c — poll serial and PS/2 keyboard for a character. */
#include "input.h"

#include "keyboard.h"
#include "serial.h"

/* Bounded wait for the next serial byte. A real terminal sends an arrow key as
 * one burst (ESC [ A), so the rest of it is already waiting; the budget only
 * stops a lone ESC from blocking the shell. */
static bool serial_wait(void) {
    for (int spin = 0; spin < 200000; spin++) {
        if (serial_rx_ready()) return true;
    }
    return false;
}

/* Read one serial key. The VT100/xterm sequences become the same SHELL_KEY_*
 * codes the PS/2 path produces: ESC [ A/B/C/D are the arrows, ESC [ H and ESC [ F
 * are Home and End, and ESC [ 3 ~ is Delete. Other sequences are dropped (-1); a
 * lone ESC is returned as 0x1b. */
static int serial_key(void) {
    int c = serial_read();
    if (c != 0x1b) return c;
    if (!serial_wait()) return 0x1b;
    if (serial_read() != '[') return -1;
    if (!serial_wait()) return -1;
    int final = serial_read();
    if (final == 'A') return SHELL_KEY_UP;
    if (final == 'B') return SHELL_KEY_DOWN;
    if (final == 'C') return SHELL_KEY_RIGHT;
    if (final == 'D') return SHELL_KEY_LEFT;
    if (final == 'H') return SHELL_KEY_HOME;
    if (final == 'F') return SHELL_KEY_END;
    if (final == '3') {                  /* ESC [ 3 ~: the '~' follows, so take it too */
        if (serial_wait()) serial_read();
        return SHELL_KEY_DELETE;
    }
    return -1;
}

int input_getchar(void) {
    if (serial_rx_ready()) {
        return serial_key();
    }
    return keyboard_read();
}
