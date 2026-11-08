/* ps2_mouse.h — the PS/2 mouse (the "aux" port of the i8042 controller).
 *
 * QEMU's default pointer, and the one a VNC or GUI window drives, is a PS/2
 * mouse, not USB. This driver reads its three-byte packets and reports them as
 * the same usb_hid_mouse_t events the USB mouse produces, so the desktop has one
 * pointer path.
 */
#ifndef HELIX_PS2_MOUSE_H
#define HELIX_PS2_MOUSE_H

#include <stdbool.h>
#include <stdint.h>

#include "usb_hid.h"

/* Enable the aux port and turn on data reporting. Safe to call when no mouse is
 * attached: it then reports no mouse and the other functions do nothing. */
void ps2_mouse_init(void);

/* Whether a PS/2 mouse answered during init. */
bool ps2_mouse_present(void);

/* Take one mouse byte that the keyboard path found on the controller (status bit
 * 5 set). Assembles packets and accumulates motion. */
void ps2_mouse_feed(uint8_t byte);

/* Collect all pending mouse motion since the last call. Returns false when there
 * was nothing. dx/dy are summed, buttons are the latest state. */
bool ps2_mouse_get_event(usb_hid_mouse_t *event);

#endif /* HELIX_PS2_MOUSE_H */
