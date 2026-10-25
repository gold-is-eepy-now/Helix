/* keyboard.c — PS/2 keyboard driver using scancode set 1.
 *
 * The PS/2 controller exposes two ports:
 *   0x64  status/command  (bit 0 = output buffer full, bit 5 = mouse byte)
 *   0x60  data             (the scancode)
 *
 * We poll. A "make" code is a key press; a "break" code (bit 7 set) is a
 * release. Shift and Caps Lock modify the translation. Arrow keys arrive as
 * an 0xE0 prefix followed by another scancode; we consume and ignore them for
 * now.
 */
#include "keyboard.h"

#include <stdbool.h>
#include <stddef.h>

#include "io.h"

#define KBD_STATUS 0x64u
#define KBD_DATA   0x60u

static bool shift_down;
static bool ctrl_down;
static bool extended; /* saw the 0xE0 prefix */
static int pressed_keys[256];
#define HID_QUEUE_SIZE 64u
static keyboard_event_t hid_queue[HID_QUEUE_SIZE];
static unsigned hid_head, hid_tail;
static uint8_t hid_previous_modifiers;
static uint8_t hid_previous_usages[6];
static int hid_previous_keys[6];
static bool hid_shift;
static bool hid_ctrl;

/* Unshifted US layout, indexed by make code. */
static const char keymap_normal[128] = {
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4',
    [0x06] = '5', [0x07] = '6', [0x08] = '7', [0x09] = '8',
    [0x0A] = '9', [0x0B] = '0', [0x0C] = '-', [0x0D] = '=',
    [0x0E] = 0x7F /* Backspace */, [0x0F] = '\t',
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r',
    [0x14] = 't', [0x15] = 'y', [0x16] = 'u', [0x17] = 'i',
    [0x18] = 'o', [0x19] = 'p', [0x1A] = '[', [0x1B] = ']',
    [0x1C] = '\n',
    [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f',
    [0x22] = 'g', [0x23] = 'h', [0x24] = 'j', [0x25] = 'k',
    [0x26] = 'l', [0x27] = ';', [0x28] = '\'', [0x29] = '`',
    [0x2B] = '\\',
    [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v',
    [0x30] = 'b', [0x31] = 'n', [0x32] = 'm',
    [0x33] = ',', [0x34] = '.', [0x35] = '/',
    [0x39] = ' ',
};

/* Shifted US layout. */
static const char keymap_shift[128] = {
    [0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$',
    [0x06] = '%', [0x07] = '^', [0x08] = '&', [0x09] = '*',
    [0x0A] = '(', [0x0B] = ')', [0x0C] = '_', [0x0D] = '+',
    [0x0E] = 0x7F, [0x0F] = '\t',
    [0x10] = 'Q', [0x11] = 'W', [0x12] = 'E', [0x13] = 'R',
    [0x14] = 'T', [0x15] = 'Y', [0x16] = 'U', [0x17] = 'I',
    [0x18] = 'O', [0x19] = 'P', [0x1A] = '{', [0x1B] = '}',
    [0x1C] = '\n',
    [0x1E] = 'A', [0x1F] = 'S', [0x20] = 'D', [0x21] = 'F',
    [0x22] = 'G', [0x23] = 'H', [0x24] = 'J', [0x25] = 'K',
    [0x26] = 'L', [0x27] = ':', [0x28] = '"', [0x29] = '~',
    [0x2B] = '|',
    [0x2C] = 'Z', [0x2D] = 'X', [0x2E] = 'C', [0x2F] = 'V',
    [0x30] = 'B', [0x31] = 'N', [0x32] = 'M',
    [0x33] = '<', [0x34] = '>', [0x35] = '?',
    [0x39] = ' ',
};

void keyboard_init(void) {
    shift_down = false;
    ctrl_down = false;
    extended = false;
    for (size_t i = 0; i < sizeof(pressed_keys) / sizeof(pressed_keys[0]); i++)
        pressed_keys[i] = 0;
    hid_head = hid_tail = 0;
    hid_previous_modifiers = 0;
    hid_shift = hid_ctrl = false;
    for (size_t i = 0; i < 6; i++) {
        hid_previous_usages[i] = 0;
        hid_previous_keys[i] = 0;
    }
}

static void hid_push(int key, bool pressed) {
    unsigned next = (hid_head + 1u) % HID_QUEUE_SIZE;
    if (next == hid_tail) return;
    hid_queue[hid_head].key = key;
    hid_queue[hid_head].pressed = pressed;
    hid_head = next;
}

static int hid_translate(uint8_t usage, bool shift, bool ctrl) {
    int key = 0;
    if (usage >= 0x04u && usage <= 0x1Du) {
        key = (shift ? 'A' : 'a') + (usage - 0x04u);
    } else if (usage >= 0x1Eu && usage <= 0x27u) {
        static const char normal[] = "1234567890";
        static const char shifted[] = "!@#$%^&*()";
        key = shift ? shifted[usage - 0x1Eu] : normal[usage - 0x1Eu];
    } else {
        switch (usage) {
            case 0x28: key = KBD_KEY_ENTER; break;
            case 0x29: key = KBD_KEY_ESCAPE; break;
            case 0x2A: key = 0x7F; break;
            case 0x2B: key = '\t'; break;
            case 0x2C: key = ' '; break;
            case 0x2D: key = shift ? '_' : '-'; break;
            case 0x2E: key = shift ? '+' : '='; break;
            case 0x2F: key = shift ? '{' : '['; break;
            case 0x30: key = shift ? '}' : ']'; break;
            case 0x31: key = shift ? '|' : '\\'; break;
            case 0x33: key = shift ? ':' : ';'; break;
            case 0x34: key = shift ? '"' : '\''; break;
            case 0x35: key = shift ? '~' : '`'; break;
            case 0x36: key = shift ? '<' : ','; break;
            case 0x37: key = shift ? '>' : '.'; break;
            case 0x38: key = shift ? '?' : '/'; break;
            case 0x4F: key = KBD_KEY_RIGHT; break;
            case 0x50: key = KBD_KEY_LEFT; break;
            case 0x51: key = KBD_KEY_DOWN; break;
            case 0x52: key = KBD_KEY_UP; break;
            case 0x4A: key = KBD_KEY_HOME; break;
            case 0x4D: key = KBD_KEY_END; break;
            case 0x4C: key = KBD_KEY_DELETE; break;
            default: break;
        }
    }
    if (ctrl && key >= 'a' && key <= 'z') key &= 0x1F;
    if (ctrl && key >= 'A' && key <= 'Z') key &= 0x1F;
    return key;
}

void keyboard_hid_boot_report(uint8_t modifiers, const uint8_t usages[6]) {
    if (usages == NULL) return;
    static const uint8_t modifier_masks[8] = {1, 2, 4, 8, 16, 32, 64, 128};
    for (unsigned i = 0; i < 8; i++) {
        bool was = (hid_previous_modifiers & modifier_masks[i]) != 0;
        bool now = (modifiers & modifier_masks[i]) != 0;
        if (was == now) continue;
        int key = i < 2 ? KBD_KEY_CTRL : i < 4 ? KBD_KEY_SHIFT : KBD_KEY_ALT;
        hid_push(key, now);
    }
    hid_shift = (modifiers & 0x22u) != 0;
    hid_ctrl = (modifiers & 0x11u) != 0;

    for (unsigned i = 0; i < 6; i++) {
        uint8_t old_usage = hid_previous_usages[i];
        if (old_usage != 0) {
            bool still_down = false;
            for (unsigned j = 0; j < 6; j++)
                if (usages[j] == old_usage) still_down = true;
            if (!still_down && hid_previous_keys[i] != 0)
                hid_push(hid_previous_keys[i], false);
        }
    }
    for (unsigned i = 0; i < 6; i++) {
        uint8_t usage = usages[i];
        if (usage == 0 || usage == 1 || usage == 2 || usage == 3) continue;
        bool was_down = false;
        for (unsigned j = 0; j < 6; j++)
            if (hid_previous_usages[j] == usage) was_down = true;
        if (!was_down) {
            int key = hid_translate(usage, hid_shift, hid_ctrl);
            if (key != 0) hid_push(key, true);
        }
    }
    for (unsigned i = 0; i < 6; i++) {
        hid_previous_usages[i] = usages[i];
        hid_previous_keys[i] = usages[i] <= 3 ? 0 :
            hid_translate(usages[i], hid_shift, hid_ctrl);
    }
    hid_previous_modifiers = modifiers;
}

static int extended_key(uint8_t code) {
    switch (code) {
        case 0x48: return KBD_KEY_UP;
        case 0x50: return KBD_KEY_DOWN;
        case 0x4B: return KBD_KEY_LEFT;
        case 0x4D: return KBD_KEY_RIGHT;
        case 0x1C: return KBD_KEY_ENTER;
        case 0x1D: return KBD_KEY_CTRL;
        case 0x38: return KBD_KEY_ALT;
        case 0x53: return KBD_KEY_DELETE;
        case 0x47: return KBD_KEY_HOME;
        case 0x4F: return KBD_KEY_END;
        case 0x49: return KBD_KEY_PAGE_UP;
        case 0x51: return KBD_KEY_PAGE_DOWN;
        default: return 0;
    }
}

bool keyboard_read_event(keyboard_event_t *event) {
    if (event == NULL) return false;
    if (hid_tail != hid_head) {
        *event = hid_queue[hid_tail];
        hid_tail = (hid_tail + 1u) % HID_QUEUE_SIZE;
        return true;
    }
    uint8_t status = inb(KBD_STATUS);
    if ((status & 0x01) == 0) {
        return false;
    }

    uint8_t scancode = inb(KBD_DATA);

    /* Bit 5 means the byte came from the mouse, not the keyboard. */
    if (status & 0x20) {
        return false;
    }

    if (scancode == 0xE0) {
        extended = true;
        return false;
    }
    bool ext = extended;
    extended = false;
    bool pressed = (scancode & 0x80u) == 0;
    uint8_t code = (uint8_t)(scancode & 0x7fu);
    unsigned slot = (unsigned)code | (ext ? 0x80u : 0u);
    int key = 0;

    if (ext) {
        key = pressed_keys[slot];
        if (pressed) key = extended_key(code);
        if (key == 0) return false;
    } else if (code == 0x2A || code == 0x36) {
        key = KBD_KEY_SHIFT;
        shift_down = pressed;
    } else if (code == 0x1D) {
        key = KBD_KEY_CTRL;
        ctrl_down = pressed;
    } else if (code == 0x38) {
        key = KBD_KEY_ALT;
    } else if (code == 0x01) {
        key = KBD_KEY_ESCAPE;
    } else if (pressed) {
        key = shift_down ? (unsigned char)keymap_shift[code]
                         : (unsigned char)keymap_normal[code];
        if (ctrl_down && key >= 'a' && key <= 'z') key &= 0x1F;
    } else {
        key = pressed_keys[slot];
    }

    if (pressed) pressed_keys[slot] = key;
    else pressed_keys[slot] = 0;
    event->key = key;
    event->pressed = pressed;
    return key != 0;
}

int keyboard_read(void) {
    keyboard_event_t event;
    if (!keyboard_read_event(&event) || !event.pressed) return -1;
    if (event.key == '\n') return '\n';
    if (event.key == KBD_KEY_ENTER) return '\n';
    if (event.key == KBD_KEY_ESCAPE) return 0x1b;
    if (event.key >= 0x20 && event.key <= 0x7e) return event.key;
    if (event.key == 0x7f) return 0x7f;
    if (event.key == '\t') return '\t';
    return -1;
}
