/* usb-hid-unit.c — host-side tests for the HID boot-protocol decoder.
 *
 * usb_hid.c is pure logic (reports in, key events out), so it can be compiled
 * for the host and fed hand-made reports. The QEMU test (tests/usb-test.py) proves
 * the USB transport works; this file proves the decoding is right.
 * Built and run by `make test`.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "usb_hid.h"
/* The decoder only needs a clock for auto-repeat. The test drives it by hand. */
static uint64_t now_ms;
uint64_t time_uptime_ms(void) { return now_ms; }

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static void kb(uint8_t mods, uint8_t a, uint8_t b, uint8_t c) {
    uint8_t r[8] = {mods, 0, a, b, c, 0, 0, 0};
    usb_hid_keyboard_report(r, 8);
}
/* Pop everything queued; returns count and fills out[]. */
static int drain(keyboard_event_t *out, int max) {
    int n = 0; keyboard_event_t e;
    while (usb_hid_get_keyboard_event(&e)) { if (n < max) out[n] = e; n++; }
    return n;
}
static int last_key_pressed(int want_key, int want_pressed, keyboard_event_t *ev, int n) {
    for (int i = 0; i < n; i++) if (ev[i].key == want_key && ev[i].pressed == want_pressed) return 1;
    return 0;
}

int main(void) {
    keyboard_event_t ev[16]; int n;
    /* 'a' press then release */
    kb(0, 0x04, 0, 0); n = drain(ev, 16);
    CHECK(n == 1 && ev[0].key == 'a' && ev[0].pressed, "press a (n=%d)", n);
    kb(0, 0, 0, 0); n = drain(ev, 16);
    CHECK(n == 1 && ev[0].key == 'a' && !ev[0].pressed, "release a (n=%d)", n);

    /* Shift + a -> 'A' */
    kb(0x02, 0x04, 0, 0); n = drain(ev, 16);
    CHECK(last_key_pressed('A', 1, ev, n), "shift+a gives A");
    kb(0x00, 0, 0, 0); n = drain(ev, 16);
    CHECK(last_key_pressed('A', 0, ev, n), "release of A matches its press even after shift lifted");

    /* Caps Lock toggles letters */
    kb(0, 0x39, 0, 0); drain(ev, 16); kb(0, 0, 0, 0); drain(ev, 16);
    kb(0, 0x05, 0, 0); n = drain(ev, 16);
    CHECK(last_key_pressed('B', 1, ev, n), "caps lock b gives B");
    kb(0, 0, 0, 0); drain(ev, 16);
    kb(0x02, 0x05, 0, 0); n = drain(ev, 16);
    CHECK(last_key_pressed('b', 1, ev, n), "caps+shift b gives lowercase b");
    kb(0, 0, 0, 0); drain(ev, 16);
    kb(0, 0x39, 0, 0); drain(ev, 16); kb(0, 0, 0, 0); drain(ev, 16);   /* caps off */

    /* digits and shifted digits */
    kb(0, 0x1E, 0, 0); n = drain(ev, 16); CHECK(last_key_pressed('1', 1, ev, n), "1");
    kb(0, 0, 0, 0); drain(ev, 16);
    kb(0x02, 0x1E, 0, 0); n = drain(ev, 16); CHECK(last_key_pressed('!', 1, ev, n), "shift+1 = !");
    kb(0, 0, 0, 0); drain(ev, 16);
    kb(0, 0x27, 0, 0); n = drain(ev, 16); CHECK(last_key_pressed('0', 1, ev, n), "0");
    kb(0, 0, 0, 0); drain(ev, 16);

    /* Enter, Escape, Backspace, Tab, Space */
    kb(0, 0x28, 0, 0); n = drain(ev, 16); CHECK(last_key_pressed('\n', 1, ev, n), "enter -> newline");
    kb(0, 0, 0, 0); drain(ev, 16);
    kb(0, 0x29, 0, 0); n = drain(ev, 16); CHECK(last_key_pressed(KBD_KEY_ESCAPE, 1, ev, n), "escape");
    kb(0, 0, 0, 0); drain(ev, 16);
    kb(0, 0x2A, 0, 0); n = drain(ev, 16); CHECK(last_key_pressed(0x7F, 1, ev, n), "backspace -> 0x7f");
    kb(0, 0, 0, 0); drain(ev, 16);
    kb(0, 0x2C, 0, 0); n = drain(ev, 16); CHECK(last_key_pressed(' ', 1, ev, n), "space");
    kb(0, 0, 0, 0); drain(ev, 16);

    /* Arrows */
    kb(0, 0x50, 0, 0); n = drain(ev, 16); CHECK(last_key_pressed(KBD_KEY_LEFT, 1, ev, n), "left arrow");
    kb(0, 0, 0, 0); drain(ev, 16);
    kb(0, 0x52, 0, 0); n = drain(ev, 16); CHECK(last_key_pressed(KBD_KEY_UP, 1, ev, n), "up arrow");
    kb(0, 0, 0, 0); drain(ev, 16);

    /* Modifier events: Ctrl down and up */
    kb(0x01, 0, 0, 0); n = drain(ev, 16); CHECK(last_key_pressed(KBD_KEY_CTRL, 1, ev, n), "ctrl down");
    kb(0x00, 0, 0, 0); n = drain(ev, 16); CHECK(last_key_pressed(KBD_KEY_CTRL, 0, ev, n), "ctrl up");

    /* Rollover report is ignored (no events, no state change) */
    uint8_t roll[8] = {0, 0, 1, 1, 1, 1, 1, 1};
    usb_hid_keyboard_report(roll, 8); n = drain(ev, 16); CHECK(n == 0, "rollover ignored (n=%d)", n);

    /* Auto-repeat: hold 'x', wait past the delay, expect repeats; release stops them */
    now_ms = 1000;
    kb(0, 0x1B, 0, 0); n = drain(ev, 16); CHECK(n == 1 && ev[0].key == 'x', "x pressed");
    now_ms = 1400; n = drain(ev, 16); CHECK(n == 0, "no repeat before 500 ms (n=%d)", n);
    now_ms = 1600; n = drain(ev, 16); CHECK(n >= 1 && ev[0].key == 'x' && ev[0].pressed, "repeat after 500 ms (n=%d)", n);
    kb(0, 0, 0, 0); drain(ev, 16);
    now_ms = 5000; n = drain(ev, 16); CHECK(n == 0, "repeats stop after key up (n=%d)", n);

    /* Mouse boot report: buttons, dx, dy (signed) */
    uint8_t m1[3] = {0x01, 5, (uint8_t)-5};
    usb_hid_mouse_report(m1, 3);
    uint8_t m2[3] = {0x00, 3, 2};
    usb_hid_mouse_report(m2, 3);
    usb_hid_mouse_t mo;
    CHECK(usb_hid_get_mouse_event(&mo) && mo.dx == 8 && mo.dy == -3 && mo.buttons == 0,
          "mouse accumulates motion (dx=%d dy=%d b=%d)", mo.dx, mo.dy, mo.buttons);
    uint32_t kr = 0, mr = 0; usb_hid_report_counts(&kr, &mr);
    CHECK(mr == 2 && kr > 0, "report counters (kbd=%u mouse=%u)", kr, mr);

    printf("%s: %d failure(s)\n", fails ? "FAILED" : "all HID decoder checks passed", fails);
    return fails != 0;
}
