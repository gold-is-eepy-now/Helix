/* desktop.h — a basic Windows 98-style desktop on the framebuffer. */
#ifndef HELIX_DESKTOP_H
#define HELIX_DESKTOP_H

#include <stdbool.h>

/* Run the desktop until the user chooses Shut Down (or presses Esc). Returns
 * false, without drawing, if there is no framebuffer. */
bool desktop_run(void);

#endif /* HELIX_DESKTOP_H */
