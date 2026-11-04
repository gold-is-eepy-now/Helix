/* usb_hid.h — USB HID boot-protocol decoders (keyboard and mouse).
 *
 * The USB host driver (usb_host.c) collects 8-byte keyboard reports and 3-byte
 * mouse reports from interrupt endpoints and passes them here. This module turns
 * them into the same keyboard_event_t values the PS/2 driver produces, so the
 * shell cannot tell the two apart.
 */
#ifndef HELIX_USB_HID_H
#define HELIX_USB_HID_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "keyboard.h"

typedef struct {
    int16_t dx;        /* accumulated relative motion since the last read */
    int16_t dy;
    uint8_t buttons;   /* bit 0 left, bit 1 right, bit 2 middle (latest state) */
} usb_hid_mouse_t;

/* One boot keyboard report: [0] modifier bits, [1] reserved, [2..7] key usages. */
void usb_hid_keyboard_report(const uint8_t *report, size_t length);

/* One boot mouse report: [0] buttons, [1] dx, [2] dy (signed), optional wheel. */
void usb_hid_mouse_report(const uint8_t *report, size_t length);

/* Pop one decoded key event (press, release, or software auto-repeat). */
bool usb_hid_get_keyboard_event(keyboard_event_t *event);

/* Read and clear accumulated mouse motion. Returns false when nothing changed. */
bool usb_hid_get_mouse_event(usb_hid_mouse_t *event);

/* Reports received since boot, per device class. Used by the `usb` command and
 * by the USB test, which needs to tell USB input apart from PS/2 input. */
void usb_hid_report_counts(uint32_t *keyboard_reports, uint32_t *mouse_reports);

#endif /* HELIX_USB_HID_H */
