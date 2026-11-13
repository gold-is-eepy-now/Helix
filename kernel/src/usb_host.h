/* usb_host.h — USB host controllers: UHCI root ports, hubs, and HID devices.
 *
 * Boot-time only for now: devices are enumerated once, when usb_host_init() runs.
 * Hot-plug and hub status-change interrupts are not handled yet.
 */
#ifndef HELIX_USB_HOST_H
#define HELIX_USB_HOST_H

#include <stdbool.h>
#include <stdint.h>

/* Start every USB host controller PCI reports and enumerate what is attached.
 * Returns true when at least one UHCI controller is running. Call once at boot,
 * after the timer and the physical frame allocator are up. */
bool usb_host_init(uint64_t hhdm_offset);

/* Print controllers, hubs and HID devices (the `usb` shell command). */
void usb_host_print_status(void);

/* Collect finished HID reports and feed them to usb_hid. Cheap when no HID
 * device is attached. Called from the keyboard input path, so it is polled. */
void usb_host_poll(void);

/* What the USB stack found, for the sign-in screen: controllers, devices, HID keyboards
 * and mice, and how many reports and errors each HID interface has produced. */
typedef struct {
    unsigned controllers, devices, keyboards, mice;
    uint32_t keyboard_reports, keyboard_errors, mouse_reports, mouse_errors;
} usb_summary_t;

void usb_host_summary(usb_summary_t *out);

/* One enumerated device, for the sign-in screen's device list. */
#define USB_INFO_PATH_MAX 8u
typedef struct {
    uint16_t vid, pid;
    uint8_t dev_class;
    bool is_hub, keyboard, mouse;
    uint16_t controller_io;               /* I/O base of the UHCI companion it hangs off */
    uint8_t path_len;
    uint8_t path[USB_INFO_PATH_MAX];      /* root port, then hub ports */
} usb_device_info_t;

/* Fill out for device i (0 <= i < usb_summary().devices). False when i is out of range. */
bool usb_host_device_info(unsigned i, usb_device_info_t *out);

#endif /* HELIX_USB_HOST_H */
