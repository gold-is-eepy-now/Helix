/* input.c — poll serial and PS/2 keyboard for a character. */
#include "input.h"

#include "keyboard.h"
#include "serial.h"

int input_getchar(void) {
    if (serial_rx_ready()) {
        return serial_read();
    }
    return keyboard_read();
}
