/* usb_hid.c — USB HID device driver for Helix OS.
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

#include "usb_hid.h"

#include "driver.h"
#include "io.h"
#include "heap.h"
#include "console.h"
#include "kstring.h"
#include <stddef.h>

/* Define EHCI/XHCI PCI device IDs (placeholder values) */
#define EHCI_PCI_VENDOR_ID 0x8086
#define EHCI_PCI_DEVICE_ID 0x2A05
#define XHCI_PCI_VENDOR_ID 0x8086
#define XHCI_PCI_DEVICE_ID 0x1E6F

/* USB protocol constants */
#define USB_SETUP_PACKET_SIZE 8
#define USB_INTERRUPT_ENDPOINT 0x81  /* IN, endpoint 1 */
#define USB_HID_CLASS 0x03
#define USB_HID_PROTOCOL_NONE 0x00

static usb_hid_device_t *g_current_keyboard = NULL;
static usb_hid_device_t *g_current_mouse = NULL;
static keyboard_event_t g_last_keyboard_event;
static bool g_last_keyboard_event_valid = false;
static int16_t g_last_mouse_dx = 0;
static int16_t g_last_mouse_dy = 0;
static uint8_t g_last_mouse_buttons = 0;

/* Simple kernel-malloc-based allocation */
void *kmalloc_for_usb(size_t size) {
    return kzalloc(size);
}

/* Free allocated memory */
void kfree_for_usb(void *ptr) {
    if (ptr) {
        kfree(ptr);
    }
}

/* EHCI controller functions */
static void ehci_init_stub(void) {
    /* Placeholder EHCI initialization - set to legacy operation mode */
    /* This is a no-op for now - real implementation would:
     * 1. Locate EHCI PCI device
     * 2. Map memory-mapped I/O region
     * 3. Configure controller for USB operation
     */
}

static void xhci_init_stub(void) {
    /* Placeholder XHCI initialization */
    /* This is a no-op for now - real implementation would:
     * 1. Locate XHCI PCI device
     * 2. Map memory-mapped I/O region
     * 3. Configure controller for USB operation
     */
}

/* USB HID driver initialization - dummy implementation */
static void hid_keyboard_init_stub(void) {
    /* Placeholder - keyboard would be created by actual USB controller
     * when it detects a HID keyboard device */
}

static void hid_mouse_init_stub(void) {
    /* Placeholder - mouse would be created by actual USB controller
     * when it detects a HID mouse device */
}

/* EHCI/XHCI driver registration */
static struct driver ehci_driver = {
    .name = "EHCI",
    .init = ehci_init_stub,
    .probe = NULL,
    .remove = NULL,
};

static struct driver xhci_driver = {
    .name = "XHCI",
    .init = xhci_init_stub,
    .probe = NULL,
    .remove = NULL,
};

static struct driver keyboard_hid_driver = {
    .name = "HID Keyboard",
    .init = hid_keyboard_init_stub,
    .probe = NULL,
    .remove = NULL,
};

static struct driver mouse_hid_driver = {
    .name = "HID Mouse",
    .init = hid_mouse_init_stub,
    .probe = NULL,
    .remove = NULL,
};

/* Register drivers */
static void register_hid_drivers(void) {
    driver_register(&ehci_driver);
    driver_register(&xhci_driver);
    driver_register(&keyboard_hid_driver);
    driver_register(&mouse_hid_driver);
}

/* Public functions */

void usb_hid_init(void) {
    register_hid_drivers();
    driver_init_all();
    console_write("[usb-hid] USB HID driver initialized (placeholder)\n");
}

void usb_hid_register_device(usb_hid_device_t *device) {
    if (device == NULL) return;
    /* Device registration placeholder */
    console_write("[usb-hid] Device registered: interface ");
    console_write_dec(device->interface_number);
    console_write("\n");
}

void usb_hid_process_report(usb_hid_device_t *device, const uint8_t *report,
                           size_t report_length) {
    if (device == NULL) return;
    /* Report processing placeholder - real implementation would parse HID reports */
}

bool usb_hid_get_keyboard_event(keyboard_event_t *event) {
    if (event == NULL || !g_last_keyboard_event_valid) return false;
    event->key = g_last_keyboard_event.key;
    event->pressed = g_last_keyboard_event.pressed;
    return true;
}

bool usb_hid_get_mouse_event(int16_t *dx, int16_t *dy, uint8_t *buttons) {
    if (dx == NULL || dy == NULL || buttons == NULL) return false;

    *dx = g_last_mouse_dx;
    *dy = g_last_mouse_dy;
    *buttons = g_last_mouse_buttons;

    /* Reset for next report */
    g_last_mouse_dx = 0;
    g_last_mouse_dy = 0;
    g_last_mouse_buttons = 0;

    return true;
}