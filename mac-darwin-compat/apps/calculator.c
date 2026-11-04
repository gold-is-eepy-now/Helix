/* calculator.c — Mac Calculator application implementation.
 *
 * Implements standard macOS Calculator UI, operations, and AppKit controls.
 *
 * Part of Helix Mac/Darwin compatibility subsystem.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include "../include/mac_appkit_x11.h"

typedef struct {
    double current_value;
    double accumulator;
    char   display_text[32];
    char   pending_op;
    bool   entering_decimal;
    bool   clear_on_next_digit;
    ns_window_t *window;
    ns_view_t   *display_label;
} mac_calc_app_t;

static mac_calc_app_t g_calc;

static void update_display(mac_calc_app_t *calc) {
    if (calc->display_label) {
        strncpy(calc->display_label->title, calc->display_text, sizeof(calc->display_label->title) - 1);
    }
}

static void on_digit_click(ns_view_t *sender, void *context) {
    (void)context;
    mac_calc_app_t *c = &g_calc;
    if (c->clear_on_next_digit) {
        c->display_text[0] = '\0';
        c->clear_on_next_digit = false;
    }

    size_t len = strlen(c->display_text);
    if (len < 16 && sender->title[0] != '\0') {
        c->display_text[len] = sender->title[0];
        c->display_text[len + 1] = '\0';
    }
    update_display(c);
}

static void on_op_click(ns_view_t *sender, void *context) {
    (void)context;
    mac_calc_app_t *c = &g_calc;
    char op = sender->title[0];

    double current = atof(c->display_text);

    if (op == 'C') {
        c->current_value = 0;
        c->accumulator = 0;
        c->pending_op = 0;
        strcpy(c->display_text, "0");
        c->clear_on_next_digit = true;
    } else if (op == '=') {
        if (c->pending_op == '+') c->accumulator += current;
        else if (c->pending_op == '-') c->accumulator -= current;
        else if (c->pending_op == '*' || c->pending_op == 'x') c->accumulator *= current;
        else if (c->pending_op == '/' && current != 0) c->accumulator /= current;
        else c->accumulator = current;

        snprintf(c->display_text, sizeof(c->display_text), "%g", c->accumulator);
        c->pending_op = 0;
        c->clear_on_next_digit = true;
    } else {
        c->accumulator = current;
        c->pending_op = op;
        c->clear_on_next_digit = true;
    }

    update_display(c);
}

int mac_app_calculator_init(void) {
    memset(&g_calc, 0, sizeof(g_calc));
    strcpy(g_calc.display_text, "0");
    g_calc.clear_on_next_digit = true;

    /* Window: width 232, height 320, title "Calculator" */
    ns_window_t *win = ns_window_create(100, 100, 232, 320, "Calculator");
    if (!win) return -1;
    g_calc.window = win;

    /* LCD Display */
    ns_view_t *disp = ns_label_create(10, 36, 212, 44, "0");
    disp->bg_color = 0xFF202020; /* Dark LCD background */
    disp->text_color = 0xFFFFFFFF;
    ns_window_add_subview(win, disp);
    g_calc.display_label = disp;

    /* Calculator keypad layout */
    const char *btn_labels[5][4] = {
        { "C", "+/-", "%", "/" },
        { "7", "8",   "9", "*" },
        { "4", "5",   "6", "-" },
        { "1", "2",   "3", "+" },
        { "0", ".",   "=", ""  }
    };

    int start_y = 90;
    int btn_w = 48;
    int btn_h = 40;
    int gap = 6;

    for (int r = 0; r < 5; r++) {
        for (int col = 0; col < 4; col++) {
            const char *label = btn_labels[r][col];
            if (label[0] == '\0') continue;

            int x = 10 + col * (btn_w + gap);
            int y = start_y + r * (btn_h + gap);
            int w = btn_w;

            /* '0' button is double width */
            if (r == 4 && col == 0) {
                w = btn_w * 2 + gap;
                col++; /* Skip next column */
            }

            ns_action_callback_t cb = (label[0] >= '0' && label[0] <= '9') ? on_digit_click : on_op_click;
            ns_view_t *btn = ns_button_create(x, y, w, btn_h, label, cb, NULL);

            /* macOS orange operator buttons */
            if (col == 3 || strcmp(label, "=") == 0) {
                btn->bg_color = 0xFFFF9F0A; /* Mac operator orange */
                btn->text_color = 0xFFFFFFFF;
            } else if (r == 0) {
                btn->bg_color = 0xFFA5A5A5; /* Mac light grey function buttons */
                btn->text_color = 0xFF000000;
            } else {
                btn->bg_color = 0xFF333333; /* Dark keypad buttons */
                btn->text_color = 0xFFFFFFFF;
            }

            ns_window_add_subview(win, btn);
        }
    }

    ns_window_order_front(win);
    return 0;
}
