/* input_shim.c — Linux input devices over Helix's keyboard and mouse queues.
 *
 * A driver (applespi) registers an input device, declares which keys and axes it reports,
 * and then calls input_report_*() and input_sync(). This file turns those reports into the
 * events the rest of Helix already consumes: key presses into the keyboard queue, and
 * pointer motion into the relative-mouse accumulator, both in usb_hid.c.
 *
 * Honest limits:
 *  - Only the keys named in the linux/input.h enum have a Helix code; others are dropped.
 *  - Helix's input_device_t objects (kernel/src/input_device.c) are not fed; the shell and
 *    desktop read usb_hid's queues, so that is where these events land.
 *  - A touchpad moves the pointer from its first finger only, scaled by POINTER_DIVISOR.
 *    The divisor is a guess chosen so a finger's travel is usable; it is not calibrated. */
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/slab.h>
#include <linux/compat_common.h>

#include "keyboard.h"
#include "usb_hid.h"

#define POINTER_DIVISOR 16
#define MAX_INPUT_DEVICES 4

static struct input_dev *g_devices[MAX_INPUT_DEVICES];
static int g_device_count;

static bool is_pointer(const struct input_dev *dev) {
    return test_bit(INPUT_PROP_POINTER, dev->propbit);
}

/* Map a Linux key name to the Helix key it types. Returns 0 for keys with no mapping. The
 * shift state picks the shifted symbol for letters and the number row only. */
static int helix_key_for(unsigned int code, bool shift) {
    if (code >= KEY_A && code <= KEY_Z) {
        static const char letters[] = "abcdefghijklmnopqrstuvwxyz";
        /* KEY_A.. are not contiguous alphabetically in the enum, so look the letter up by name */
        static const unsigned int names[26] = {
            KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J, KEY_K, KEY_L, KEY_M,
            KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z,
        };
        for (int i = 0; i < 26; i++) {
            if (names[i] == code) return shift ? (letters[i] - 32) : letters[i];
        }
        return 0;
    }
    switch (code) {
        case KEY_0: return shift ? ')' : '0';
        case KEY_1: return shift ? '!' : '1';
        case KEY_2: return shift ? '@' : '2';
        case KEY_3: return shift ? '#' : '3';
        case KEY_4: return shift ? '$' : '4';
        case KEY_5: return shift ? '%' : '5';
        case KEY_6: return shift ? '^' : '6';
        case KEY_7: return shift ? '&' : '7';
        case KEY_8: return shift ? '*' : '8';
        case KEY_9: return shift ? '(' : '9';
        case KEY_SPACE: return ' ';
        case KEY_ENTER: return '\n';
        case KEY_TAB: return '\t';
        case KEY_BACKSPACE: return 0x7F;
        case KEY_ESC: return KBD_KEY_ESCAPE;
        case KEY_UP: return KBD_KEY_UP;
        case KEY_DOWN: return KBD_KEY_DOWN;
        case KEY_LEFT: return KBD_KEY_LEFT;
        case KEY_RIGHT: return KBD_KEY_RIGHT;
        case KEY_HOME: return KBD_KEY_HOME;
        case KEY_END: return KBD_KEY_END;
        case KEY_PAGEUP: return KBD_KEY_PAGE_UP;
        case KEY_PAGEDOWN: return KBD_KEY_PAGE_DOWN;
        case KEY_DELETE: return KBD_KEY_DELETE;
        case KEY_MINUS: return shift ? '_' : '-';
        case KEY_EQUAL: return shift ? '+' : '=';
        case KEY_LEFTBRACE: return shift ? '{' : '[';
        case KEY_RIGHTBRACE: return shift ? '}' : ']';
        case KEY_BACKSLASH: return shift ? '|' : '\\';
        case KEY_SEMICOLON: return shift ? ':' : ';';
        case KEY_APOSTROPHE: return shift ? '"' : '\'';
        case KEY_GRAVE: return shift ? '~' : '`';
        case KEY_COMMA: return shift ? '<' : ',';
        case KEY_DOT: return shift ? '>' : '.';
        case KEY_SLASH: return shift ? '?' : '/';
        default: return 0;
    }
}

/* Modifiers are not characters: they become KBD_KEY_SHIFT/CTRL/ALT transitions. */
static int modifier_index(unsigned int code) {
    switch (code) {
        case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT: return 0;
        case KEY_LEFTCTRL: case KEY_RIGHTCTRL: return 1;
        case KEY_LEFTALT: case KEY_RIGHTALT: return 2;
        default: return -1;
    }
}

static int modifier_key(int index) {
    return index == 0 ? KBD_KEY_SHIFT : index == 1 ? KBD_KEY_CTRL : KBD_KEY_ALT;
}

/* A modifier is announced on the first press and the last release, not per physical key. */
static void modifier_change(struct input_dev *dev, int index, int value) {
    if (value) {
        if (dev->held[index]++ == 0) usb_hid_inject_key(modifier_key(index), true);
    } else if (dev->held[index] > 0) {
        if (--dev->held[index] == 0) usb_hid_inject_key(modifier_key(index), false);
    }
}

static bool shift_held(const struct input_dev *dev) {
    return dev->held[0] > 0;
}

/* ---- the public input API ---- */

struct input_dev *input_allocate_device(void) {
    struct input_dev *dev = kzalloc(sizeof(*dev), GFP_KERNEL);
    return dev;
}

struct input_dev *devm_input_allocate_device(struct device *parent) {
    struct input_dev *dev = input_allocate_device();
    if (dev) dev->dev.acpi_companion = parent ? parent->acpi_companion : NULL;
    return dev;
}

int input_register_device(struct input_dev *dev) {
    if (dev == NULL || g_device_count >= MAX_INPUT_DEVICES) return -1;
    dev->registered = true;
    g_devices[g_device_count++] = dev;
    kprintf("[compat] input: registered '%s' (%s)\n", dev->name ? dev->name : "?",
            is_pointer(dev) ? "pointer" : "keyboard");
    return 0;
}

void input_unregister_device(struct input_dev *dev) {
    if (dev) dev->registered = false;
}

void input_set_capability(struct input_dev *dev, unsigned int type, unsigned int code) {
    if (type == EV_KEY && code < KEY_COUNT + 1000) __set_bit(code, dev->keybit);
    if (type == EV_ABS && code < ABS_COUNT) __set_bit(code, dev->absbit);
    if (type == EV_REL && code < REL_COUNT) __set_bit(code, dev->absbit);
    if (type < EV_TYPE_COUNT) __set_bit(type, dev->evbit);
}

void input_set_abs_params(struct input_dev *dev, unsigned int axis, int min, int max, int fuzz, int flat) {
    if (axis >= ABS_COUNT) return;
    dev->absinfo[axis].minimum = min;
    dev->absinfo[axis].maximum = max;
    dev->absinfo[axis].fuzz = fuzz;
    dev->absinfo[axis].flat = flat;
    __set_bit(axis, dev->absbit);
    __set_bit(EV_ABS, dev->evbit);
}

void input_abs_set_res(struct input_dev *dev, unsigned int axis, int val) {
    if (axis < ABS_COUNT) dev->absinfo[axis].resolution = val;
}

void input_report_key(struct input_dev *dev, unsigned int code, int value) {
    if (dev == NULL) return;
    if (code == BTN_LEFT) {
        dev->btn_left = value != 0;
        return;
    }
    int mod = modifier_index(code);
    if (mod >= 0) {
        modifier_change(dev, mod, value);
        return;
    }
    if (value == 2) return;     /* auto-repeat: Helix does its own repeat in the keyboard path */
    int key = helix_key_for(code, shift_held(dev));
    if (key != 0) usb_hid_inject_key(key, value != 0);
}

void input_report_abs(struct input_dev *dev, unsigned int code, int value) {
    if (dev == NULL || dev->mt == NULL) return;
    struct input_mt *mt = dev->mt;
    if (mt->cur_slot < 0 || mt->cur_slot >= mt->num_slots) return;
    struct input_mt_slot *slot = &mt->slots[mt->cur_slot];
    if (code == ABS_MT_POSITION_X) slot->x = value;
    if (code == ABS_MT_POSITION_Y) slot->y = value;
}

void input_report_rel(struct input_dev *dev, unsigned int code, int value) {
    if (dev == NULL) return;
    if (code == REL_X) usb_hid_inject_mouse(value, 0, dev->btn_left ? 1 : 0);
    if (code == REL_Y) usb_hid_inject_mouse(0, value, dev->btn_left ? 1 : 0);
}

void input_event(struct input_dev *dev, unsigned int type, unsigned int code, int value) {
    if (type == EV_KEY) input_report_key(dev, code, value);
    else if (type == EV_ABS) input_report_abs(dev, code, value);
    else if (type == EV_REL) input_report_rel(dev, code, value);
}

/* End of one report. Pointer devices move the pointer by the first finger's travel; the
 * left-button state rides along on every report so a click is never lost. */
void input_sync(struct input_dev *dev) {
    if (dev == NULL || !is_pointer(dev) || dev->mt == NULL) return;
    struct input_mt *mt = dev->mt;
    struct input_mt_slot *first = &mt->slots[0];
    int dx = 0, dy = 0;
    if (first->present && mt->was_present[0]) {
        dx = (first->x - mt->last[0].x) / POINTER_DIVISOR;
        dy = (first->y - mt->last[0].y) / POINTER_DIVISOR;
    }
    if (first->present) {
        mt->last[0].x = (s16)first->x;
        mt->last[0].y = (s16)first->y;
    }
    mt->was_present[0] = first->present;
    if (dx != 0 || dy != 0 || dev->btn_left) {
        usb_hid_inject_mouse(dx, dy, dev->btn_left ? 1 : 0);
    }
}

/* ---- multitouch slots ---- */

int input_mt_init_slots(struct input_dev *dev, unsigned int num_slots, unsigned int flags) {
    if (dev == NULL || num_slots == 0 || num_slots > INPUT_MT_MAX_SLOTS) return -1;
    if (dev->mt == NULL) dev->mt = kzalloc(sizeof(*dev->mt), GFP_KERNEL);
    if (dev->mt == NULL) return -1;
    dev->mt->num_slots = (int)num_slots;
    dev->mt->cur_slot = 0;
    dev->mt->flags = flags;
    dev->mt->next_id = 1;
    return 0;
}

void input_mt_slot(struct input_dev *dev, int slot) {
    if (dev && dev->mt && slot >= 0 && slot < dev->mt->num_slots) {
        dev->mt->cur_slot = slot;
        dev->mt->used[slot] = true;
    }
}

void input_mt_report_slot_state(struct input_dev *dev, unsigned int tool_type, bool active) {
    (void)tool_type;
    if (dev == NULL || dev->mt == NULL) return;
    struct input_mt *mt = dev->mt;
    struct input_mt_slot *slot = &mt->slots[mt->cur_slot];
    slot->present = active;
    if (active && slot->tracking_id <= 0) slot->tracking_id = mt->next_id++;
    if (!active) slot->tracking_id = -1;
}

/* After a frame: slots that were not touched this frame are no longer present. */
void input_mt_sync_frame(struct input_dev *dev) {
    if (dev == NULL || dev->mt == NULL) return;
    struct input_mt *mt = dev->mt;
    for (int i = 0; i < mt->num_slots; i++) {
        if (!mt->used[i]) mt->slots[i].present = false;
        mt->used[i] = false;
    }
}

/* Give each contact the nearest free slot to where it was last frame (within dmax), else the
 * first free slot. This is the simple form of Linux's tracking; it keeps a finger's slot while
 * it moves, which is what the driver depends on. */
int input_mt_assign_slots(struct input_dev *dev, int *slots, const struct input_mt_pos *pos, int num_pos,
                          int dmax) {
    if (dev == NULL || dev->mt == NULL || slots == NULL || pos == NULL) return -1;
    struct input_mt *mt = dev->mt;
    bool taken[INPUT_MT_MAX_SLOTS] = { false };
    for (int c = 0; c < num_pos; c++) {
        int best = -1;
        long best_d = 0;
        for (int s = 0; s < mt->num_slots; s++) {
            if (taken[s] || !mt->was_present[s]) continue;
            long dx = (long)pos[c].x - mt->last[s].x;
            long dy = (long)pos[c].y - mt->last[s].y;
            long d = dx * dx + dy * dy;
            if (d <= (long)dmax * dmax && (best < 0 || d < best_d)) {
                best = s;
                best_d = d;
            }
        }
        if (best < 0) {
            for (int s = 0; s < mt->num_slots; s++) {
                if (!taken[s] && !mt->was_present[s]) { best = s; break; }
            }
        }
        if (best < 0) return -1;
        taken[best] = true;
        slots[c] = best;
    }
    return 0;
}

void input_shim_dump(void) {
    for (int i = 0; i < g_device_count; i++) {
        kprintf("[compat] input device %d: %s\n", i, g_devices[i]->name ? g_devices[i]->name : "?");
    }
}
