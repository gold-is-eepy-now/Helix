/* input_selftest.c — the `inputtest` shell command's checks. Each check drives the real
 * shim path (driver-style reports in, Helix queue events out) and prints one line; the
 * final line is "inputtest: PASS (n failure(s))" or "inputtest: FAILED (n failure(s))". */
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/spi/spi.h>
#include <linux/errno.h>
#include <linux/compat_common.h>
#include "keyboard.h"
#include "usb_hid.h"
#include "spi.h"   /* Helix's kernel SPI layer: the fake controller */

int compat_spi_driver_count(void);
int compat_spi_probe_calls(void);

static int g_failures;

static void check(bool ok, const char *what) {
    kprintf("inputtest: %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_failures++;
}

/* Drain the keyboard queue so earlier events cannot satisfy a check. */
static void drain_keys(void) {
    keyboard_event_t ev;
    while (usb_hid_get_keyboard_event(&ev)) { }
}

static bool next_key(int *key, bool *pressed) {
    keyboard_event_t ev;
    if (!usb_hid_get_keyboard_event(&ev)) return false;
    *key = ev.key;
    *pressed = ev.pressed;
    return true;
}

static void test_keyboard(void) {
    struct input_dev *dev = devm_input_allocate_device(NULL);
    check(dev != NULL, "keyboard device allocated");
    if (!dev) return;
    dev->name = "helix-selftest-keyboard";
    input_set_capability(dev, EV_KEY, KEY_A);
    input_set_capability(dev, EV_KEY, KEY_LEFTSHIFT);
    input_register_device(dev);
    drain_keys();

    input_report_key(dev, KEY_A, 1);
    input_sync(dev);
    int key = -1; bool pressed = false;
    bool got = next_key(&key, &pressed);
    check(got && key == 'a' && pressed, "KEY_A press types 'a'");
    input_report_key(dev, KEY_A, 0);
    input_sync(dev);
    got = next_key(&key, &pressed);
    check(got && key == 'a' && !pressed, "KEY_A release reports 'a' released");

    input_report_key(dev, KEY_LEFTSHIFT, 1);
    input_sync(dev);
    got = next_key(&key, &pressed);
    check(got && key == KBD_KEY_SHIFT && pressed, "left shift press reports the shift key");
    input_report_key(dev, KEY_A, 1);
    input_sync(dev);
    got = next_key(&key, &pressed);
    check(got && key == 'A' && pressed, "KEY_A with shift held types 'A'");
    input_report_key(dev, KEY_A, 0);
    input_report_key(dev, KEY_LEFTSHIFT, 0);
    input_sync(dev);
    drain_keys();
}

static void test_touchpad(void) {
    struct input_dev *dev = devm_input_allocate_device(NULL);
    check(dev != NULL, "touchpad device allocated");
    if (!dev) return;
    dev->name = "helix-selftest-touchpad";
    __set_bit(INPUT_PROP_POINTER, dev->propbit);
    __set_bit(INPUT_PROP_BUTTONPAD, dev->propbit);
    input_set_abs_params(dev, ABS_MT_POSITION_X, 0, 10000, 0, 0);
    input_set_abs_params(dev, ABS_MT_POSITION_Y, 0, 10000, 0, 0);
    input_mt_init_slots(dev, 2, INPUT_MT_POINTER);
    input_register_device(dev);

    uint32_t before_kb = 0, before_mouse = 0;
    usb_hid_report_counts(&before_kb, &before_mouse);

    /* frame 1: one finger down at (1000, 1000); no motion yet */
    input_mt_slot(dev, 0);
    input_mt_report_slot_state(dev, MT_TOOL_FINGER, true);
    input_report_abs(dev, ABS_MT_POSITION_X, 1000);
    input_report_abs(dev, ABS_MT_POSITION_Y, 1000);
    input_mt_sync_frame(dev);
    input_sync(dev);

    /* frame 2: the same finger moves 160 units right; 160 / 16 = 10 pixels */
    input_mt_slot(dev, 0);
    input_mt_report_slot_state(dev, MT_TOOL_FINGER, true);
    input_report_abs(dev, ABS_MT_POSITION_X, 1160);
    input_report_abs(dev, ABS_MT_POSITION_Y, 1000);
    input_mt_sync_frame(dev);
    input_sync(dev);

    int16_t dx = 0, dy = 0;
    usb_hid_mouse_t m;
    bool moved = usb_hid_get_mouse_event(&m);
    dx = m.dx; dy = m.dy;
    check(moved && dx == 10 && dy == 0, "touchpad finger moving 160 units moves the pointer 10 px");

    uint32_t after_kb = 0, after_mouse = 0;
    usb_hid_report_counts(&after_kb, &after_mouse);
    check(after_mouse > before_mouse, "pointer motion is counted as a mouse report");
    (void)before_kb; (void)after_kb;
}

static void test_applespi_refuses(void) {
    /* A device with no ACPI node (not bound to the namespace) must reach applespi_probe. Its SIEN
     * lookup then starts at the root and is not found, so the probe refuses with -ENODEV. */
    check(compat_spi_driver_count() >= 1, "applespi driver registered at boot");
    struct spi_device spi = { 0 };
    int before = compat_spi_probe_calls();
    int rc = compat_spi_add_device(&spi, "APP000D");
    check(compat_spi_probe_calls() == before + 1, "APP000D device reaches applespi_probe");
    check(rc == -ENODEV, "applespi refuses without ACPI: probe returns -ENODEV");
}

/* The test build loads a synthetic SSDT (tests/gen-applespi-ssdt.py) with a device APP000D
 * named \_SB.APPT. Binding it must reach the probe, and the probe must get past its ACPI method
 * steps: the GPE is installed and enabled, and the keyboard is registered. No SPI transfer has
 * run, so the keyboard cannot send keys yet. */
static void test_applespi_binds(void) {
    int before = compat_spi_probe_calls();
    int rc = compat_spi_scan_acpi("APP000D");
    check(rc != -ENOENT, "the AML namespace has an APP000D device to bind");
    check(compat_spi_probe_calls() == before + 1, "the bound APP000D device reaches applespi_probe");
    check(rc == 0, "applespi's ACPI steps succeed (GPE installed and enabled, keyboard registered)");
}

/* ---- SPI bridge: spi_async -> compat_spi_pump -> kernel SPI layer ---- */

/* A fake controller that echoes the complement of tx, so the test can see the bytes moved.
 * A first byte of 0xEE makes it time out, to check error propagation. */
static uint8_t g_fake_cs;
static uint8_t g_fake_mode;
static spi_status_t fake_set_mode(spi_controller_t *c, uint8_t cs, uint8_t mode,
                                  uint32_t speed_hz, uint8_t bits) {
    (void)c; (void)speed_hz;
    if (bits != 8u) return SPI_ERR_BAD_ARG;
    g_fake_cs = cs;
    g_fake_mode = mode;
    return SPI_OK;
}
static spi_status_t fake_transfer(spi_controller_t *c, uint8_t cs, const uint8_t *tx,
                                  uint8_t *rx, uint32_t len) {
    (void)c; (void)cs;
    if (tx != NULL && len > 0 && tx[0] == 0xEEu) return SPI_ERR_TIMEOUT;
    for (uint32_t i = 0; i < len && rx != NULL; i++) {
        rx[i] = tx != NULL ? (uint8_t)(tx[i] ^ 0xFFu) : 0xFFu;
    }
    return SPI_OK;
}
static spi_controller_t g_fake = {
    .name = "selftest-spi", .ready = true,
    .set_mode = fake_set_mode, .transfer = fake_transfer, .priv = NULL,
};

/* Queue one transfer and run the pump. Returns the pump's count of completed messages. */
static int run_one(struct spi_message *m, struct spi_transfer *t) {
    spi_message_init(m);
    spi_message_add_tail(t, m);
    if (spi_async(NULL, m) != 0) return -1;
    return compat_spi_pump();
}

/* Test hooks from spi_shim.c: lend the bridge to a controller, then take the binding back. */
void compat_spi_test_begin(spi_controller_t *c);
void compat_spi_test_end(void);

/* With no slave linked, a message completes with -ENODEV and sends nothing. This holds before
 * the applespi scan and after it, because the message's device is not the linked one. */
static void test_spi_unlinked(void) {
    struct spi_message m;
    struct spi_transfer t = { 0 };
    uint8_t buf[2] = { 1, 2 };
    t.tx_buf = buf;
    t.len = 2;
    t.cs_change = 1;
    run_one(&m, &t);
    check(m.status == -ENODEV && m.actual_length == 0,
          "SPI message from an unlinked device completes with -ENODEV, 0 bytes");
}

/* Lends the bridge to the fake controller for these checks only. The fake is not added to the
 * kernel's controller table, so the real LPSS controller is never affected. */
static void test_spi_fake_controller(void) {
    compat_spi_test_begin(&g_fake);

    /* A 4-byte message: the reply is the complement of tx, and cs 0 / mode 0 reach the controller. */
    uint8_t tx[4] = { 0x10, 0x20, 0x30, 0x40 };
    uint8_t rx[4] = { 0, 0, 0, 0 };
    struct spi_message m;
    struct spi_transfer t = { 0 };
    t.tx_buf = tx; t.rx_buf = rx; t.len = 4; t.cs_change = 1;
    run_one(&m, &t);
    check(m.status == 0 && m.actual_length == 4 && rx[0] == 0xEF && rx[3] == 0xBF,
          "fake controller: 4-byte message completes with status 0 and the reply");
    check(g_fake_cs == 0u && g_fake_mode == 0u, "chip select 0 and mode 0 reach the controller");

    /* A controller timeout must reach the message as -EIO with no bytes counted. */
    tx[0] = 0xEE;
    run_one(&m, &t);
    check(m.status == -EIO && m.actual_length == 0, "controller timeout completes the message with -EIO");

    /* Holding chip select across two transfers is refused before anything is sent. */
    uint8_t a[1] = { 1 }, b[1] = { 2 };
    struct spi_transfer t1 = { 0 }, t2 = { 0 };
    t1.tx_buf = a; t1.len = 1; t1.cs_change = 0;
    t2.tx_buf = b; t2.len = 1; t2.cs_change = 1;
    struct spi_message m2;
    spi_message_init(&m2);
    spi_message_add_tail(&t1, &m2);
    spi_message_add_tail(&t2, &m2);
    spi_async(NULL, &m2);
    compat_spi_pump();
    check(m2.status == -EOPNOTSUPP && m2.actual_length == 0,
          "holding chip select across transfers is refused with -EOPNOTSUPP");

    compat_spi_test_end();
}

int compat_input_selftest(void) {
    g_failures = 0;
    test_keyboard();
    test_touchpad();
    test_spi_unlinked();
    test_applespi_refuses();
    test_applespi_binds();
    test_spi_fake_controller();
    if (g_failures == 0) {
        kprintf("inputtest: PASS (0 failure(s))\n");
    } else {
        kprintf("inputtest: FAILED (%d failure(s))\n", g_failures);
    }
    return g_failures;
}
