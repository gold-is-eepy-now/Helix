/* keyboard.h — PS/2 keyboard input (polling, no interrupts). */
#ifndef HELIX_KEYBOARD_H
#define HELIX_KEYBOARD_H

/* Nothing to configure yet, but keep the call site stable for later. */
void keyboard_init(void);

/* Read one translated character, or -1 if nothing is waiting. Never blocks.
 * Returns printable ASCII, '\n' for Enter, 0x7F for Backspace, '\t' for Tab. */
int keyboard_read(void);

#endif /* HELIX_KEYBOARD_H */
