/* ps2_mouse.c — PS/2 mouse driver (see ps2_mouse.h).
 *
 * The i8042 controller has two devices: the keyboard on port 0x60, and an "aux"
 * device (the mouse). Commands to the mouse are sent through the controller with
 * the 0xD4 prefix, and the mouse answers with 0xFA (ACK). Its packets arrive on
 * port 0x60 with status bit 5 set, which is how the keyboard path tells them apart.
 *
 * Packet layout (standard PS/2, three bytes):
 *   byte 0: bit 3 always 1 (used to resync), bit 0 left, bit 1 right,
 *           bit 2 middle, bit 4 X sign, bit 5 Y sign, bits 6/7 overflow.
 *   byte 1: X movement (low 8 bits). byte 2: Y movement (low 8 bits).
 * Y grows upward on a PS/2 mouse and downward on the screen, so it is negated.
 */
#include "ps2_mouse.h"

#include <stdbool.h>
#include <stdint.h>

#include "io.h"
#include "klog.h"

#define I8042_DATA   0x60u
#define I8042_STATUS 0x64u
#define STATUS_OUT_FULL  0x01u   /* output buffer has a byte to read */
#define STATUS_IN_FULL   0x02u   /* input buffer still busy with our last byte */
#define STATUS_AUX       0x20u   /* the byte in the output buffer is from the mouse */

#define CMD_ENABLE_AUX   0xA8u
#define CMD_READ_CONFIG  0x20u
#define CMD_WRITE_CONFIG 0x60u
#define CMD_TO_AUX       0xD4u   /* the next data byte goes to the mouse */

#define MOUSE_ACK        0xFAu
#define MOUSE_DEFAULTS   0xF6u
#define MOUSE_ENABLE     0xF4u

static bool g_present;
static uint8_t g_packet[3];
static int g_index;
static uint8_t g_buttons;
static int32_t g_pending_dx;
static int32_t g_pending_dy;
static bool g_has_motion;

/* Bounded spins: a missing controller must not hang the boot. */
static bool wait_input_clear(void) {
    for (uint32_t i = 0; i < 100000u; i++) {
        if ((inb(I8042_STATUS) & STATUS_IN_FULL) == 0) return true;
    }
    return false;
}

static bool wait_output_full(void) {
    for (uint32_t i = 0; i < 100000u; i++) {
        if (inb(I8042_STATUS) & STATUS_OUT_FULL) return true;
    }
    return false;
}

static bool controller_write(uint8_t port, uint8_t value) {
    if (!wait_input_clear()) return false;
    outb(port, value);
    return true;
}

/* Send one byte to the mouse and wait for its ACK. */
static bool mouse_command(uint8_t value) {
    if (!controller_write(I8042_STATUS, CMD_TO_AUX)) return false;
    if (!controller_write(I8042_DATA, value)) return false;
    if (!wait_output_full()) return false;
    return inb(I8042_DATA) == MOUSE_ACK;
}

void ps2_mouse_init(void) {
    g_present = false;
    g_index = 0;
    g_buttons = 0;
    g_pending_dx = 0;
    g_pending_dy = 0;
    g_has_motion = false;

    if (!controller_write(I8042_STATUS, CMD_ENABLE_AUX)) {
        klog_note(KLOG_INPUT, "[ps2] controller did not accept the aux enable\n");
        return;
    }
    /* Controller config: turn off the aux IRQ (bit 1), because input is polled.
     * Bit 5 (aux clock disabled) is cleared so the mouse gets a clock. The
     * keyboard's IRQ and clock bits are left as they were. */
    if (!controller_write(I8042_STATUS, CMD_READ_CONFIG) || !wait_output_full()) return;
    uint8_t config = inb(I8042_DATA);
    config = (uint8_t)((config & ~(0x02u | 0x20u)));
    if (!controller_write(I8042_STATUS, CMD_WRITE_CONFIG) ||
        !controller_write(I8042_DATA, config)) {
        return;
    }

    if (!mouse_command(MOUSE_DEFAULTS) || !mouse_command(MOUSE_ENABLE)) {
        klog_note(KLOG_INPUT, "[ps2] no PS/2 mouse answered\n");
        return;
    }
    g_present = true;
    klog_note(KLOG_INPUT, "[ps2] PS/2 mouse enabled\n");
}

bool ps2_mouse_present(void) {
    return g_present;
}

void ps2_mouse_feed(uint8_t byte) {
    /* Resync: a packet's first byte always has bit 3 set. Anything else is noise
     * or a lost byte, so skip it until a valid first byte arrives. */
    if (g_index == 0 && (byte & 0x08u) == 0) return;

    g_packet[g_index++] = byte;
    if (g_index < 3) return;
    g_index = 0;

    uint8_t status = g_packet[0];
    if (status & 0xC0u) return;            /* X or Y overflowed: drop the packet */

    int32_t dx = g_packet[1];
    int32_t dy = g_packet[2];
    if (status & 0x10u) dx -= 256;         /* the sign bits extend the 8-bit values */
    if (status & 0x20u) dy -= 256;

    g_pending_dx += dx;
    g_pending_dy += -dy;                   /* PS/2 Y is up; screen Y is down */
    g_buttons = status & 0x07u;
    g_has_motion = true;
}

/* Pull every mouse byte waiting in the controller. Stops at a keyboard byte, which
 * the keyboard driver must read. */
static void drain(void) {
    for (int i = 0; i < 64; i++) {
        uint8_t status = inb(I8042_STATUS);
        if ((status & STATUS_OUT_FULL) == 0) break;
        if ((status & STATUS_AUX) == 0) break;
        ps2_mouse_feed(inb(I8042_DATA));
    }
}

static int16_t clamp16(int32_t v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

bool ps2_mouse_get_event(usb_hid_mouse_t *event) {
    if (!g_present || event == NULL) return false;
    drain();
    if (!g_has_motion) return false;
    event->dx = clamp16(g_pending_dx);
    event->dy = clamp16(g_pending_dy);
    event->buttons = g_buttons;
    g_pending_dx = 0;
    g_pending_dy = 0;
    g_has_motion = false;
    return true;
}
