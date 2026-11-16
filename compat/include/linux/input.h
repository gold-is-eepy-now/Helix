/* linux/input.h — Linux-style input device interface, over Helix's input device objects.
 *
 * Part of the Linux compatibility layer. Drivers describe a device (name, which event
 * types and codes it can produce), register it, and report events with input_report_*();
 * compat_input.c turns those into events on a Helix input_device_t.
 *
 * The event type and code names (EV_KEY, KEY_A, ABS_MT_POSITION_X, ...) are Helix's own
 * enumerations. They share Linux's NAMES so driver source compiles, but not its numbers,
 * so nothing here may be assumed to match Linux's userspace ABI.
 *
 * Only the keyboard and multitouch-touchpad subset a real driver (applespi) needs exists;
 * see docs/08-linux-compat.md for what is and is not implemented. */
#ifndef HELIX_COMPAT_LINUX_INPUT_H
#define HELIX_COMPAT_LINUX_INPUT_H

#include <linux/types.h>
#include <linux/device.h>
#include <linux/compat_common.h>

enum {
    EV_SYN = 0, EV_KEY, EV_REL, EV_ABS, EV_LED, EV_REP, EV_TYPE_COUNT
};
enum { LED_CAPSL = 1, LED_COUNT = 16 };
enum { BUS_SPI = 0x1C };
enum { BTN_LEFT = 1000, BTN_RIGHT, BTN_TOUCH };
enum {
    ABS_MT_SLOT = 0, ABS_MT_TOUCH_MAJOR, ABS_MT_TOUCH_MINOR, ABS_MT_WIDTH_MAJOR,
    ABS_MT_WIDTH_MINOR, ABS_MT_ORIENTATION, ABS_MT_POSITION_X, ABS_MT_POSITION_Y,
    ABS_MT_TRACKING_ID, ABS_COUNT
};
enum { INPUT_PROP_POINTER = 0, INPUT_PROP_BUTTONPAD, INPUT_PROP_COUNT };
enum { MT_TOOL_FINGER = 0 };
enum {
    KEY_0,
    KEY_1,
    KEY_102ND,
    KEY_2,
    KEY_3,
    KEY_4,
    KEY_5,
    KEY_6,
    KEY_7,
    KEY_8,
    KEY_9,
    KEY_A,
    KEY_APOSTROPHE,
    KEY_B,
    KEY_BACKSLASH,
    KEY_BACKSPACE,
    KEY_BRIGHTNESSDOWN,
    KEY_BRIGHTNESSUP,
    KEY_C,
    KEY_CAPSLOCK,
    KEY_COMMA,
    KEY_D,
    KEY_DASHBOARD,
    KEY_DELETE,
    KEY_DOT,
    KEY_DOWN,
    KEY_E,
    KEY_END,
    KEY_ENTER,
    KEY_EQUAL,
    KEY_ESC,
    KEY_F,
    KEY_F1,
    KEY_F10,
    KEY_F11,
    KEY_F12,
    KEY_F2,
    KEY_F3,
    KEY_F4,
    KEY_F5,
    KEY_F6,
    KEY_F7,
    KEY_F8,
    KEY_F9,
    KEY_FN,
    KEY_G,
    KEY_GRAVE,
    KEY_H,
    KEY_HOME,
    KEY_I,
    KEY_INSERT,
    KEY_J,
    KEY_K,
    KEY_KATAKANAHIRAGANA,
    KEY_KBDILLUMDOWN,
    KEY_KBDILLUMUP,
    KEY_L,
    KEY_LEFT,
    KEY_LEFTALT,
    KEY_LEFTBRACE,
    KEY_LEFTCTRL,
    KEY_LEFTMETA,
    KEY_LEFTSHIFT,
    KEY_M,
    KEY_MINUS,
    KEY_MUHENKAN,
    KEY_MUTE,
    KEY_N,
    KEY_NEXTSONG,
    KEY_O,
    KEY_P,
    KEY_PAGEDOWN,
    KEY_PAGEUP,
    KEY_PLAYPAUSE,
    KEY_PREVIOUSSONG,
    KEY_Q,
    KEY_R,
    KEY_RIGHT,
    KEY_RIGHTALT,
    KEY_RIGHTBRACE,
    KEY_RIGHTMETA,
    KEY_RIGHTSHIFT,
    KEY_RO,
    KEY_S,
    KEY_SCALE,
    KEY_SEMICOLON,
    KEY_SLASH,
    KEY_SPACE,
    KEY_T,
    KEY_TAB,
    KEY_U,
    KEY_UP,
    KEY_V,
    KEY_VOLUMEDOWN,
    KEY_VOLUMEUP,
    KEY_W,
    KEY_X,
    KEY_Y,
    KEY_YEN,
    KEY_Z,
    KEY_COUNT
};

struct input_id {
    u16 bustype, vendor, product, version;
};

struct input_absinfo {
    s32 minimum, maximum;
    s32 fuzz, flat, resolution;
};

struct input_dev {
    const char *name;
    const char *phys;
    struct input_id id;
    unsigned long evbit[1];
    unsigned long ledbit[1];
    unsigned long propbit[1];
    struct input_absinfo absinfo[ABS_COUNT];
    unsigned long absbit[1];
    unsigned long keybit[(KEY_COUNT + 1000 + 63) / 64];
    void *drvdata;
    /* Called when the system sets an LED or repeat parameter (Linux: input_dev->event). */
    int (*event)(struct input_dev *dev, unsigned int type, unsigned int code, int value);
    struct device dev;
    struct input_mt *mt;
    void *helix_device;            /* the input_device_t this registered as */
    bool registered;
};

struct input_dev *devm_input_allocate_device(struct device *dev);
struct input_dev *input_allocate_device(void);
int input_register_device(struct input_dev *dev);
void input_unregister_device(struct input_dev *dev);

void input_set_capability(struct input_dev *dev, unsigned int type, unsigned int code);
void input_set_abs_params(struct input_dev *dev, unsigned int axis, int min, int max, int fuzz, int flat);
void input_abs_set_res(struct input_dev *dev, unsigned int axis, int val);
void input_report_key(struct input_dev *dev, unsigned int code, int value);
void input_report_abs(struct input_dev *dev, unsigned int code, int value);
void input_report_rel(struct input_dev *dev, unsigned int code, int value);
void input_event(struct input_dev *dev, unsigned int type, unsigned int code, int value);
void input_sync(struct input_dev *dev);

static inline void input_set_drvdata(struct input_dev *dev, void *data) { dev->drvdata = data; }
static inline void *input_get_drvdata(struct input_dev *dev) { return dev->drvdata; }
static inline void input_set_abs_params_unused(void) {}

#endif /* HELIX_COMPAT_LINUX_INPUT_H */
