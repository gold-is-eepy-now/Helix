/* input_device.h — input device object for Helix OS.
 *
 * M12 milestone: input devices.
 *
 * Defines an input device object that represents keyboards, mice, and
 * other input devices. Input events can be read through the device's
 * event queue.
 *
 * This provides a foundation for an input subsystem that can be used
 * by a compositor or window manager.
 */

#ifndef HELIX_INPUT_DEVICE_H
#define HELIX_INPUT_DEVICE_H

#include "object.h"

typedef enum {
    INPUT_DEVICE_KEYBOARD,
    INPUT_DEVICE_MOUSE,
    INPUT_DEVICE_TOUCHPAD,
    INPUT_DEVICE_GAMEPAD,
} input_device_type_t;

typedef struct input_event input_event_t;

struct input_event {
    uint64_t timestamp;
    uint32_t type;
    uint32_t code;
    uint32_t value;
};

typedef struct input_device input_device_t;

struct input_device {
    input_device_type_t type;
    char name[32];
    uint8_t event_count;
    input_event_t events[32];
};

void input_device_init(input_device_t *dev, input_device_type_t type,
                       const char *name);
object_t *input_device_create(input_device_type_t type, const char *name);
void input_device_push_event(input_device_t *dev, uint32_t code,
                             uint32_t value, uint64_t timestamp);
uint8_t input_device_event_count(input_device_t *dev);

#endif /* HELIX_INPUT_DEVICE_H */