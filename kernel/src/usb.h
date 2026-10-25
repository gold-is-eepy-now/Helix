/* usb.h — device-independent USB descriptor and topology types. */
#ifndef HELIX_USB_H
#define HELIX_USB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define USB_MAX_INTERFACES 16u
#define USB_MAX_ENDPOINTS 32u
#define USB_CLASS_HUB 9u
#define USB_CLASS_HID 3u
#define USB_HID_SUBCLASS_BOOT 1u
#define USB_HID_PROTOCOL_KEYBOARD 1u

typedef struct {
    uint8_t number;
    uint8_t alternate_setting;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t protocol;
} usb_interface_info_t;

typedef struct {
    uint8_t address;
    uint8_t attributes;
    uint16_t max_packet_size;
    uint8_t interval;
    uint8_t interface_number;
} usb_endpoint_info_t;

typedef struct {
    uint16_t total_length;
    uint8_t configuration_value;
    uint8_t interface_count;
    uint8_t endpoint_count;
    bool has_hub_interface;
    bool has_boot_keyboard_interface;
    usb_interface_info_t interfaces[USB_MAX_INTERFACES];
    usb_endpoint_info_t endpoints[USB_MAX_ENDPOINTS];
} usb_configuration_info_t;

typedef struct {
    uint8_t descriptor_length;
    uint8_t port_count;
    uint16_t characteristics;
    uint8_t power_on_delay;
    uint8_t controller_current;
} usb_hub_info_t;

/* Validates a full configuration descriptor and extracts interfaces/endpoints. */
bool usb_parse_configuration(const uint8_t *bytes, size_t length,
                             usb_configuration_info_t *out);
bool usb_parse_hub_descriptor(const uint8_t *bytes, size_t length,
                              usb_hub_info_t *out);

#endif
