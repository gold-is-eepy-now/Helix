/* keyboard.c — PS/2 keyboard driver using scancode set 1.
 *
 * The PS/2 controller exposes two ports:
 *   0x64  status/command  (bit 0 = output buffer full, bit 5 = mouse byte)
 *   0x60  data             (the scancode)
 *
 * We poll. A "make" code is a key press; a "break" code (bit 7 set) is a
 * release. Shift and Caps Lock modify the translation. Arrow keys arrive as
 * an 0xE0 prefix followed by another scancode; we consume and ignore them for
 * now.
 */
#include "keyboard.h"

#include <stdbool.h>

#include "io.h"

#define KBD_STATUS 0x64u
#define KBD_DATA   0x60u

static bool shift_down;
static bool extended; /* saw the 0xE0 prefix */

/* Unshifted US layout, indexed by make code. */
static const char keymap_normal[128] = {
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4',
    [0x06] = '5', [0x07] = '6', [0x08] = '7', [0x09] = '8',
    [0x0A] = '9', [0x0B] = '0', [0x0C] = '-', [0x0D] = '=',
    [0x0E] = 0x7F /* Backspace */, [0x0F] = '\t',
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r',
    [0x14] = 't', [0x15] = 'y', [0x16] = 'u', [0x17] = 'i',
    [0x18] = 'o', [0x19] = 'p', [0x1A] = '[', [0x1B] = ']',
    [0x1C] = '\n',
    [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f',
    [0x22] = 'g', [0x23] = 'h', [0x24] = 'j', [0x25] = 'k',
    [0x26] = 'l', [0x27] = ';', [0x28] = '\'', [0x29] = '`',
    [0x2B] = '\\',
    [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v',
    [0x30] = 'b', [0x31] = 'n', [0x32] = 'm',
    [0x33] = ',', [0x34] = '.', [0x35] = '/',
    [0x39] = ' ',
};

/* Shifted US layout. */
static const char keymap_shift[128] = {
    [0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$',
    [0x06] = '%', [0x07] = '^', [0x08] = '&', [0x09] = '*',
    [0x0A] = '(', [0x0B] = ')', [0x0C] = '_', [0x0D] = '+',
    [0x0E] = 0x7F, [0x0F] = '\t',
    [0x10] = 'Q', [0x11] = 'W', [0x12] = 'E', [0x13] = 'R',
    [0x14] = 'T', [0x15] = 'Y', [0x16] = 'U', [0x17] = 'I',
    [0x18] = 'O', [0x19] = 'P', [0x1A] = '{', [0x1B] = '}',
    [0x1C] = '\n',
    [0x1E] = 'A', [0x1F] = 'S', [0x20] = 'D', [0x21] = 'F',
    [0x22] = 'G', [0x23] = 'H', [0x24] = 'J', [0x25] = 'K',
    [0x26] = 'L', [0x27] = ':', [0x28] = '"', [0x29] = '~',
    [0x2B] = '|',
    [0x2C] = 'Z', [0x2D] = 'X', [0x2E] = 'C', [0x2F] = 'V',
    [0x30] = 'B', [0x31] = 'N', [0x32] = 'M',
    [0x33] = '<', [0x34] = '>', [0x35] = '?',
    [0x39] = ' ',
};

void keyboard_init(void) {
    shift_down = false;
    extended = false;
}

int keyboard_read(void) {
    uint8_t status = inb(KBD_STATUS);
    if ((status & 0x01) == 0) {
        return -1; /* nothing to read */
    }

    uint8_t scancode = inb(KBD_DATA);

    /* Bit 5 means the byte came from the mouse, not the keyboard. */
    if (status & 0x20) {
        return -1;
    }

    if (scancode == 0xE0) {
        extended = true;
        return -1;
    }
    if (extended) {
        extended = false; /* arrow keys etc. arrive here; ignore for now */
        return -1;
    }

    if (scancode & 0x80) {
        /* Break code: a key was released. */
        uint8_t make = (uint8_t)(scancode & 0x7F);
        if (make == 0x2A || make == 0x36) {
            shift_down = false;
        }
        return -1;
    }

    if (scancode == 0x2A || scancode == 0x36) {
        shift_down = true;
        return -1;
    }

    char c = shift_down ? keymap_shift[scancode] : keymap_normal[scancode];
    return c ? (int)(unsigned char)c : -1;
}
