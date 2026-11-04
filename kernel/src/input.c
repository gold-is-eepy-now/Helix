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

/* Read one serial key. ESC [ A and ESC [ B (up and down arrows, the VT100/xterm
 * sequences) become SHELL_KEY_UP and SHELL_KEY_DOWN, the same codes the PS/2 path
 * uses. Other escape sequences are dropped (-1); a lone ESC is returned as 0x1b. */
static int serial_key(void) {
    int c = serial_read();
    if (c != 0x1b) return c;
    if (!serial_wait()) return 0x1b;
    if (serial_read() != '[') return -1;
    if (!serial_wait()) return -1;
    int final = serial_read();
    if (final == 'A') return SHELL_KEY_UP;
    if (final == 'B') return SHELL_KEY_DOWN;
    return -1;
}

int input_getchar(void) {
    if (serial_rx_ready()) {
        return serial_key();
    }
    return keyboard_read();
}
