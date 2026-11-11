/* ui.c — see ui.h. The loop is small on purpose: each screen returns when the
 * user leaves it, and this decides where to go next. */
#include "ui.h"

#include <stdbool.h>

#include "console.h"
#include "desktop.h"
#include "fb.h"
#include "shell.h"

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

void ui_run(void) {
    if (!fb_ready() || fb_width() < 640 || fb_height() < 480) {
        shell_run();   /* no usable screen: the serial shell, as on a headless machine */
    }
    /* Serial tests wait for this line before they send the switch key. */
    console_write("[ui] sign-in\n");
    for (;;) {
        desktop_run(); /* sign-in first, then the desktop */
        if (g_switch) {
            tui_shell();
        }
        /* Otherwise the user signed out or cancelled the sign-in: show it again. */
    }
}
