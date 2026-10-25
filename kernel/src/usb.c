/* usb.c — device-independent USB descriptor parsing. */
#include "usb.h"

static uint16_t read_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

bool usb_parse_configuration(const uint8_t *bytes, size_t length,
                             usb_configuration_info_t *out) {
    if (bytes == NULL || out == NULL || length < 9u || bytes[0] < 9u ||
        bytes[1] != 2u || read_le16(bytes + 2u) != length)
        return false;

    usb_configuration_info_t parsed = {0};
    parsed.total_length = (uint16_t)length;
    parsed.configuration_value = bytes[5];
    uint8_t active_interface = 0xFFu;
    size_t offset = 0;
    while (offset < length) {
        if (length - offset < 2u) return false;
        uint8_t descriptor_length = bytes[offset];
        uint8_t descriptor_type = bytes[offset + 1u];
        if (descriptor_length < 2u || descriptor_length > length - offset)
            return false;
        if (offset == 0u) {
            if (descriptor_type != 2u || descriptor_length < 9u) return false;
        } else if (descriptor_type == 4u) {
            if (descriptor_length < 9u ||
                parsed.interface_count == USB_MAX_INTERFACES)
                return false;
            usb_interface_info_t *interface_info =
                &parsed.interfaces[parsed.interface_count++];
            active_interface = bytes[offset + 2u];
            interface_info->number = active_interface;
            interface_info->alternate_setting = bytes[offset + 3u];
            interface_info->class_code = bytes[offset + 5u];
            interface_info->subclass = bytes[offset + 6u];
            interface_info->protocol = bytes[offset + 7u];
            if (interface_info->class_code == USB_CLASS_HUB)
                parsed.has_hub_interface = true;
            if (interface_info->class_code == USB_CLASS_HID &&
                interface_info->subclass == USB_HID_SUBCLASS_BOOT &&
                interface_info->protocol == USB_HID_PROTOCOL_KEYBOARD)
                parsed.has_boot_keyboard_interface = true;
        } else if (descriptor_type == 5u) {
            if (descriptor_length < 7u || active_interface == 0xFFu ||
                parsed.endpoint_count == USB_MAX_ENDPOINTS)
                return false;
            usb_endpoint_info_t *endpoint =
                &parsed.endpoints[parsed.endpoint_count++];
            endpoint->address = bytes[offset + 2u];
            endpoint->attributes = bytes[offset + 3u];
            endpoint->max_packet_size = read_le16(bytes + offset + 4u) & 0x07FFu;
            endpoint->interval = bytes[offset + 6u];
            endpoint->interface_number = active_interface;
            if (endpoint->max_packet_size == 0u) return false;
        }
        offset += descriptor_length;
    }
    if (parsed.interface_count < bytes[4]) return false;
    *out = parsed;
    return true;
}

bool usb_parse_hub_descriptor(const uint8_t *bytes, size_t length,
                              usb_hub_info_t *out) {
    if (bytes == NULL || out == NULL || length < 7u || bytes[0] < 7u ||
        bytes[1] != 0x29u || bytes[2] == 0u)
        return false;
    out->descriptor_length = bytes[0];
    out->port_count = bytes[2];
    out->characteristics = read_le16(bytes + 3u);
    out->power_on_delay = bytes[5];
    out->controller_current = bytes[6];
    return true;
}
