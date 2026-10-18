/* input.c — poll serial and PS/2 keyboard for a character. */
#include "input.h"

#include "keyboard.h"
#include "serial.h"

static unsigned int serial_escape_state;
static unsigned int serial_escape_wait;
static int deferred_serial_key = -1;

int input_getchar(void) {
    if (serial_rx_ready()) {
        return serial_read();
    }
    return keyboard_read();
}

int input_getkey(void) {
    if (deferred_serial_key >= 0) {
        int key = deferred_serial_key;
        deferred_serial_key = -1;
        return key;
    }
    if (serial_rx_ready()) {
        int c = serial_read();
        serial_escape_wait = 0;
        if (serial_escape_state == 0) {
            if (c == 0x1B) {
                serial_escape_state = 1;
                return -1;
            }
            return c;
        }
        if (serial_escape_state == 1) {
            if (c == '[') {
                serial_escape_state = 2;
                return -1;
            }
            serial_escape_state = 0;
            deferred_serial_key = c;
            return 0x1B;
        }
        serial_escape_state = 0;
        switch (c) {
            case 'A': return KBD_KEY_UP;
            case 'B': return KBD_KEY_DOWN;
            case 'C': return KBD_KEY_RIGHT;
            case 'D': return KBD_KEY_LEFT;
            case 'H': return KBD_KEY_HOME;
            case 'F': return KBD_KEY_END;
            default: return -1;
        }
    }
    if (serial_escape_state != 0 && ++serial_escape_wait > 1000000u) {
        serial_escape_state = 0;
        serial_escape_wait = 0;
        return 0x1B;
    }
    keyboard_event_t event;
    while (keyboard_read_event(&event)) {
        if (!event.pressed) continue;
        if (event.key == KBD_KEY_ENTER) return '\n';
        if (event.key == KBD_KEY_ESCAPE) return 0x1B;
        return event.key;
    }
    return -1;
}
