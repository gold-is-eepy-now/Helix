/* usb_hid.h — USB HID device driver for Helix OS.
 *
 * USB HID (Human Interface Device) driver for keyboards, mice, and other
 * HID devices. Supports USB 2.0 (EHCI/XHCI) host controllers.
 *
 * Implements the low-level USB protocol and HID report parsing for:
 * - Keyboard input (PS/2 emulation compatible)
 * - Mouse input (relative motion and buttons)
 * - General-purpose HID devices (future expansion)
 *
 * Devices are exposed as OBJECT_TYPE_INPUT_DEVICE objects with HID-specific
 * metadata stored in the device payload.
 */

#ifndef HELIX_USB_HID_H
#define HELIX_USB_HID_H

#include <stdbool.h>
#include <stdint.h>

#include "object.h"
#include "keyboard.h"

/* HID report types */
enum {
    HID_REPORT_TYPE_INPUT = 0x01,
    HID_REPORT_TYPE_OUTPUT = 0x02,
    HID_REPORT_TYPE_FEATURE = 0x03,
};

/* HID usage pages */
enum {
    HID_PAGE_GENERIC_DESKTOP = 0x01,
    HID_PAGE_KEYBOARD = 0x06,
    HID_PAGE_MOUSE = 0x02,
};

/* HID usage codes */
enum {
    /* Keyboard modifiers */
    HID_KEYBOARD_LEFT_CTRL = 0xE0,
    HID_KEYBOARD_LEFT_SHIFT = 0xE1,
    HID_KEYBOARD_LEFT_ALT = 0xE2,
    HID_KEYBOARD_LEFT_GUI = 0xE3,
    HID_KEYBOARD_RIGHT_CTRL = 0xE4,
    HID_KEYBOARD_RIGHT_SHIFT = 0xE5,
    HID_KEYBOARD_RIGHT_ALT = 0xE6,
    HID_KEYBOARD_RIGHT_GUI = 0xE7,

    /* Keyboard keys */
    HID_KEYBOARD_A = 0x04,
    HID_KEYBOARD_B = 0x05,
    HID_KEYBOARD_C = 0x06,
    HID_KEYBOARD_D = 0x07,
    /* ... more keys ... */
    HID_KEYBOARD_Z = 0x1D,

    /* Mouse buttons */
    HID_MOUSE_LEFT_BUTTON = 0x01,
    HID_MOUSE_RIGHT_BUTTON = 0x02,
    HID_MOUSE_MIDDLE_BUTTON = 0x03,

    /* Mouse motion */
    HID_MOUSE_X = 0x30,
    HID_MOUSE_Y = 0x31,
    HID_MOUSE_WHEEL = 0x38,
};

typedef struct usb_hid_device usb_hid_device_t;

struct usb_hid_device {
    object_t *device_obj;      /* Handle to the device object */
    uint8_t interface_number;  /* USB interface number */
    uint8_t report_id;         /* Current report ID */
    uint16_t report_size;      /* Size of current report */
    uint8_t *report_buffer;    /* Buffer for parsed HID report */
    size_t report_buffer_size;

    /* Device-specific data */
    union {
        struct {
            /* Keyboard state */
            uint8_t modifier_keys;
            uint8_t key_codes[6];
            size_t key_count;
        } keyboard;
        struct {
            /* Mouse state */
            uint8_t buttons;
            int16_t x;
            int16_t y;
            int16_t wheel;
        } mouse;
        /* More device types can be added here */
    } data;
};

/* Initialize the USB HID driver (called by EHCI/XHCI) */
void usb_hid_init(void);

/* Register a new USB HID device */
void usb_hid_register_device(usb_hid_device_t *device);

/* Process a HID report from a USB interrupt transfer */
void usb_hid_process_report(usb_hid_device_t *device, const uint8_t *report,
                            size_t report_length);

/* Get the keyboard event from the last processed HID report */
bool usb_hid_get_keyboard_event(keyboard_event_t *event);

/* Get the mouse event from the last processed HID report */
bool usb_hid_get_mouse_event(int16_t *dx, int16_t *dy, uint8_t *buttons);

#endif /* HELIX_USB_HID_H */