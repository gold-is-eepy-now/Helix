/* ui.c — see ui.h. The loop is small on purpose: each screen returns when the
 * user leaves it, and this decides where to go next. */
#include "ui.h"

#include <stdbool.h>

#include "console.h"
#include "desktop.h"
#include "fb.h"
#include "shell.h"
#include "time.h"

static bool g_switch;

void ui_request_switch(void) {
    g_switch = true;
}

bool ui_switch_requested(void) {
    return g_switch;
}

/* The text shell on the framebuffer. Returns when the switch key is pressed. */
static void tui_shell(void) {
    g_switch = false;
    fb_graphics_end();
    console_write("Helix TUI shell. Press Ctrl+Alt+T to return to the desktop.\n\n");
    shell_run_until_switch();
    g_switch = false;
}

/* Keep the boot log on screen this long before the sign-in screen replaces it, so it can be
 * read or photographed on a machine with no keyboard yet. Input is not read during the hold:
 * keys typed then stay queued for the sign-in screen. */
#define BOOT_HOLD_MS 15000u

static void boot_hold(void) {
    console_write("\nSign-in screen in 15 seconds. Boot log is held on screen.\n");
    uint64_t end = time_uptime_ms() + BOOT_HOLD_MS;
    while (time_uptime_ms() < end) {
        __asm__ volatile("pause");
    }
}

void ui_run(void) {
    if (!fb_ready() || fb_width() < 640 || fb_height() < 480) {
        shell_run();   /* no usable screen: the serial shell, as on a headless machine */
    }
    /* Serial tests wait for this line before they send the switch key. */
    console_write("[ui] sign-in\n");
    boot_hold();
    for (;;) {
        desktop_run(); /* sign-in first, then the desktop */
        if (g_switch) {
            tui_shell();
        }
        /* Otherwise the user signed out or cancelled the sign-in: show it again. */
    }
}
