/* usb_hid.c — decode USB HID boot-protocol reports into input events.
 *
 * Keyboards send the state of every key on each change, not press/release
 * events. So each report is compared with the previous one:
 *   a usage that is newly present is a press, one that disappeared is a release.
 * We remember what each usage produced when it was pressed, so the release
 * matches the press even if Shift was let go in between.
 *
 * Boot keyboards do not auto-repeat, so we repeat held keys in software, using
 * the same timing a PC BIOS uses (about 500 ms delay, then about 30 per second).
 */
#include "usb_hid.h"

#include "time.h"

#define HID_KEY_ERROR_ROLLOVER 0x01u
#define QUEUE_SIZE 64u
#define REPEAT_DELAY_MS 500u
#define REPEAT_PERIOD_MS 33u

/* HID modifier byte bits. */
#define MOD_LCTRL 0x01u
#define MOD_LSHIFT 0x02u
#define MOD_LALT 0x04u
#define MOD_RCTRL 0x10u
#define MOD_RSHIFT 0x20u
#define MOD_RALT 0x40u

static keyboard_event_t g_queue[QUEUE_SIZE];
static unsigned g_head, g_tail;

static uint8_t g_prev_keys[6];
static uint8_t g_prev_mods;
static int g_produced[256];          /* key emitted for each usage at press time */
static bool g_caps_lock;
static uint8_t g_repeat_usage;       /* 0 = nothing repeating */
static int g_repeat_key;
static uint64_t g_repeat_at;

static usb_hid_mouse_t g_mouse;
static uint32_t g_keyboard_reports;
static uint32_t g_mouse_reports;

static void push(int key, bool pressed) {
    unsigned next = (g_head + 1u) % QUEUE_SIZE;
    if (next == g_tail) return;      /* full: drop, the user will notice and type again */
    g_queue[g_head].key = key;
    g_queue[g_head].pressed = pressed;
    g_head = next;
}

/* Printable keys follow the US layout, like the PS/2 driver. Letters honour
 * Caps Lock and Shift together; punctuation only honours Shift. */
static int usage_to_key(uint8_t usage, bool shift, bool caps) {
    if (usage >= 0x04 && usage <= 0x1D) {
        char c = (char)('a' + (usage - 0x04));
        bool upper = shift != caps;
        return upper ? c - ('a' - 'A') : c;
    }
    static const char digits_plain[] = "1234567890";
    static const char digits_shift[] = "!@#$%^&*()";
    if (usage >= 0x1E && usage <= 0x27) {
        return shift ? digits_shift[usage - 0x1E] : digits_plain[usage - 0x1E];
    }
    switch (usage) {
        case 0x28: return '\n';                 /* Enter: same as the PS/2 path */
        case 0x29: return KBD_KEY_ESCAPE;
        case 0x2A: return 0x7F;                 /* Backspace */
        case 0x2B: return '\t';
        case 0x2C: return ' ';
        case 0x2D: return shift ? '_' : '-';
        case 0x2E: return shift ? '+' : '=';
        case 0x2F: return shift ? '{' : '[';
        case 0x30: return shift ? '}' : ']';
        case 0x31: return shift ? '|' : '\\';
        case 0x33: return shift ? ':' : ';';
        case 0x34: return shift ? '"' : '\'';
        case 0x35: return shift ? '~' : '`';
        case 0x36: return shift ? '<' : ',';
        case 0x37: return shift ? '>' : '.';
        case 0x38: return shift ? '?' : '/';
        case 0x4A: return KBD_KEY_HOME;
        case 0x4B: return KBD_KEY_PAGE_UP;
        case 0x4C: return KBD_KEY_DELETE;
        case 0x4D: return KBD_KEY_END;
        case 0x4E: return KBD_KEY_PAGE_DOWN;
        case 0x4F: return KBD_KEY_RIGHT;
        case 0x50: return KBD_KEY_LEFT;
        case 0x51: return KBD_KEY_DOWN;
        case 0x52: return KBD_KEY_UP;
        default: return 0;                      /* function keys etc. not mapped yet */
    }
}

static bool contains(const uint8_t *list, uint8_t usage) {
    for (unsigned i = 0; i < 6; i++)
        if (list[i] == usage) return true;
    return false;
}

static void handle_modifiers(uint8_t mods) {
    /* Announce a modifier only when the pair as a whole changes (left or right). */
    bool shift_before = (g_prev_mods & (MOD_LSHIFT | MOD_RSHIFT)) != 0;
    bool shift_now = (mods & (MOD_LSHIFT | MOD_RSHIFT)) != 0;
    bool ctrl_before = (g_prev_mods & (MOD_LCTRL | MOD_RCTRL)) != 0;
    bool ctrl_now = (mods & (MOD_LCTRL | MOD_RCTRL)) != 0;
    bool alt_before = (g_prev_mods & (MOD_LALT | MOD_RALT)) != 0;
    bool alt_now = (mods & (MOD_LALT | MOD_RALT)) != 0;
    if (shift_before != shift_now) push(KBD_KEY_SHIFT, shift_now);
    if (ctrl_before != ctrl_now) push(KBD_KEY_CTRL, ctrl_now);
    if (alt_before != alt_now) push(KBD_KEY_ALT, alt_now);
    g_prev_mods = mods;
}

void usb_hid_keyboard_report(const uint8_t *report, size_t length) {
    if (report == NULL || length < 8) return;
    g_keyboard_reports++;
    /* All six key slots filled with 0x01 means the keyboard lost track of its
     * keys (rollover). Ignore the report; the next normal one resynchronises. */
    bool rollover = true;
    for (unsigned i = 2; i < 8; i++)
        if (report[i] != HID_KEY_ERROR_ROLLOVER) rollover = false;
    if (rollover) return;

    uint8_t mods = report[0];
    handle_modifiers(mods);
    bool shift = (mods & (MOD_LSHIFT | MOD_RSHIFT)) != 0;

    /* Releases: usages that were held and are gone now. */
    for (unsigned i = 0; i < 6; i++) {
        uint8_t usage = g_prev_keys[i];
        if (usage == 0 || contains(&report[2], usage)) continue;
        int key = g_produced[usage];
        g_produced[usage] = 0;
        if (key != 0) push(key, false);
        if (g_repeat_usage == usage) g_repeat_usage = 0;
    }
    /* Presses: usages that are new. */
    for (unsigned i = 0; i < 6; i++) {
        uint8_t usage = report[2 + i];
        if (usage == 0 || contains(g_prev_keys, usage)) continue;
        if (usage == 0x39) {                       /* Caps Lock toggles on press */
            g_caps_lock = !g_caps_lock;
            continue;
        }
        int key = usage_to_key(usage, shift, g_caps_lock);
        if (key == 0) continue;
        g_produced[usage] = key;
        push(key, true);
        g_repeat_usage = usage;
        g_repeat_key = key;
        g_repeat_at = time_uptime_ms() + REPEAT_DELAY_MS;
    }
    for (unsigned i = 0; i < 6; i++) g_prev_keys[i] = report[2 + i];
    if (g_repeat_usage != 0 && !contains(report + 2, g_repeat_usage)) g_repeat_usage = 0;
}

bool usb_hid_get_keyboard_event(keyboard_event_t *event) {
    if (event == NULL) return false;
    if (g_repeat_usage != 0 && time_uptime_ms() >= g_repeat_at) {
        /* Repeat only while the key is still held in the latest report. */
        push(g_repeat_key, true);
        g_repeat_at = time_uptime_ms() + REPEAT_PERIOD_MS;
    }
    if (g_tail == g_head) return false;
    *event = g_queue[g_tail];
    g_tail = (g_tail + 1u) % QUEUE_SIZE;
    return true;
}

void usb_hid_mouse_report(const uint8_t *report, size_t length) {
    if (report == NULL || length < 3) return;
    g_mouse_reports++;
    g_mouse.buttons = (uint8_t)(report[0] & 0x07u);
    int dx = (int8_t)report[1];
    int dy = (int8_t)report[2];
    int sum_x = g_mouse.dx + dx;
    int sum_y = g_mouse.dy + dy;
    g_mouse.dx = (int16_t)(sum_x > 32767 ? 32767 : sum_x < -32768 ? -32768 : sum_x);
    g_mouse.dy = (int16_t)(sum_y > 32767 ? 32767 : sum_y < -32768 ? -32768 : sum_y);
}

bool usb_hid_get_mouse_event(usb_hid_mouse_t *event) {
    if (event == NULL) return false;
    *event = g_mouse;
    bool moved = g_mouse.dx != 0 || g_mouse.dy != 0;
    g_mouse.dx = 0;
    g_mouse.dy = 0;
    return moved || event->buttons != 0;
}

void usb_hid_report_counts(uint32_t *keyboard_reports, uint32_t *mouse_reports) {
    if (keyboard_reports != NULL) *keyboard_reports = g_keyboard_reports;
    if (mouse_reports != NULL) *mouse_reports = g_mouse_reports;
}
