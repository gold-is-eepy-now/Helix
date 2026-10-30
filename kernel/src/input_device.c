/* input_device.c — input device object implementation for Helix OS.
 *
 * M12 milestone: input devices.
 *
 * Provides an input device object that represents keyboards, mice, and
 * other input devices. Input events can be pushed to the device's
 * event queue.
 */

#include "input_device.h"
#include "console.h"
#include "heap.h"
#include "kstring.h"

#define MAX_INPUT_DEVICES 8

static input_device_t *g_input_devices[MAX_INPUT_DEVICES];
static uint8_t g_input_device_count;

void input_device_init(input_device_t *dev, input_device_type_t type,
                       const char *name) {
    strcpy(dev->name, name);
    dev->type = type;
    dev->event_count = 0;
}

object_t *input_device_create(input_device_type_t type, const char *name) {
    if (g_input_device_count >= MAX_INPUT_DEVICES) return NULL;

    input_device_t *dev = kzalloc(sizeof(*dev));
    if (dev == NULL) return NULL;

    strcpy(dev->name, name);
    dev->type = type;
    dev->event_count = 0;

    g_input_devices[g_input_device_count++] = dev;
    return NULL;  /* Placeholder for now */
}

void input_device_push_event(input_device_t *dev, uint32_t code,
                             uint32_t value, uint64_t timestamp) {
    if (!dev || dev->event_count >= 32) return;

    input_event_t *evt = &dev->events[dev->event_count++];
    evt->timestamp = timestamp;
    evt->type = dev->type;
    evt->code = code;
    evt->value = value;
}

uint8_t input_device_event_count(input_device_t *dev) {
    return dev ? dev->event_count : 0;
}