/* login.h — the graphical sign-in screen shown before the desktop. */
#ifndef HELIX_LOGIN_H
#define HELIX_LOGIN_H

#include <stdbool.h>

/* Show the sign-in screen and wait for the user. Returns true with the session
 * set to the signed-in account, or false if the user cancels (Esc). When no
 * accounts exist yet, the screen creates the first one instead.
 *
 * Keys: Tab moves between fields, Enter signs in, Backspace edits, Esc cancels.
 * Draws on the framebuffer; the caller must have a framebuffer ready. */
bool login_run(void);

#endif /* HELIX_LOGIN_H */
