/* ui.h — the graphical session: sign-in, desktop, and the TUI shell.
 *
 * On a machine with a framebuffer, boot goes to the sign-in screen. Ctrl+Alt+T
 * (Ctrl+T on serial) switches to the TUI shell, a text shell on the screen; the
 * same key switches back. Esc on the desktop signs out. Without a framebuffer
 * the serial shell runs as before. */
#ifndef HELIX_UI_H
#define HELIX_UI_H

#include <stdbool.h>

/* Runs the session. Never returns. */
void ui_run(void);

/* Set by the sign-in screen and the desktop when the switch key is pressed. */
void ui_request_switch(void);
bool ui_switch_requested(void);

#endif /* HELIX_UI_H */
