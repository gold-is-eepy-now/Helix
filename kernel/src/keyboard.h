/* keyboard.h — PS/2 keyboard input (polling, no interrupts). */
#ifndef HELIX_KEYBOARD_H
#define HELIX_KEYBOARD_H

#include <stdbool.h>

enum {
    KBD_KEY_UP = 0x100,
    KBD_KEY_DOWN,
    KBD_KEY_LEFT,
    KBD_KEY_RIGHT,
    KBD_KEY_ESCAPE,
    KBD_KEY_ENTER,
    KBD_KEY_SHIFT,
    KBD_KEY_CTRL,
    KBD_KEY_ALT,
    KBD_KEY_HOME,
    KBD_KEY_END,
    KBD_KEY_PAGE_UP,
    KBD_KEY_PAGE_DOWN,
    KBD_KEY_DELETE,
};

/* Codes returned by keyboard_read and input_getchar for the editing keys. They
 * sit outside the byte range, so a key code never collides with a typed character. */
/* Switches between the graphical session and the TUI shell. Ctrl+Alt+T on the
 * keyboard; Ctrl+T (0x14) arrives this way on the serial console. */
#define KBD_SWITCH_KEY   0x14
#define SHELL_KEY_UP     0x101
#define SHELL_KEY_DOWN   0x102
#define SHELL_KEY_LEFT   0x103
#define SHELL_KEY_RIGHT  0x104
#define SHELL_KEY_HOME   0x105
#define SHELL_KEY_END    0x106
#define SHELL_KEY_DELETE 0x107

typedef struct {
    int key;       /* printable ASCII or one of KBD_KEY_* */
    bool pressed;  /* false for a release */
} keyboard_event_t;

/* Nothing to configure yet, but keep the call site stable for later. */
void keyboard_init(void);

/* Read one translated character, or -1 if nothing is waiting. Never blocks.
 * Returns printable ASCII, '\n' for Enter, 0x7F for Backspace, '\t' for Tab. */
int keyboard_read(void);

/* Read one PS/2 key press/release event, or return false if none is ready. */
bool keyboard_read_event(keyboard_event_t *event);

#endif /* HELIX_KEYBOARD_H */
