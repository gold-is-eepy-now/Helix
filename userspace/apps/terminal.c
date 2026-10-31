/* terminal.c — Simple terminal emulator for Helix userspace.
 *
 * Uses Linux runtime syscalls (SYSCALL_READ_INPUT, SYSCALL_WRITE) for I/O.
 * Provides basic console functionality: command echo, line editing, history.
 * Runs in userspace and does not depend on kernel framebuffer headers.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "linux_runtime.h"

/* Buffer for line editing */
#define TERM_HIST 8
#define TERM_BUF 256

typedef struct {
    char buf[TERM_BUF];
    int pos;
    int history[TERM_HIST];
    int hist_idx;
    int hist_count;
} terminal_t;

/* Initialize terminal */
static void terminal_init(terminal_t *t) {
    memset(t, 0, sizeof(terminal_t));
    t->pos = 0;
    t->hist_idx = -1;
    t->hist_count = 0;
}

/* Handle backspace */
static void terminal_backspace(terminal_t *t) {
    if (t->pos > 0) {
        t->pos--;
        write(1, "\b \b", 3);  /* erase char */
    }
}

/* Handle newline */
static void terminal_newline(terminal_t *t) {
    write(1, "\n", 1);
    t->pos = 0;
    t->history[t->hist_count % TERM_HIST] = 0;  /* placeholder */
    t->hist_count++;
}

/* Main terminal loop */
int main(int argc, char **argv) {
    terminal_t term;
    terminal_init(&term);

    write(1, "[helix-terminal] Ready.\n", 24);

    while (1) {
        /* Print prompt */
        write(1, "[helix] $ ", 10);

        /* Read a line */
        int i = 0;
        char c;
        char buf[TERM_BUF];

        while (1) {
            int n = read(0, &c, 1);
            if (n <= 0) break;

            if (c == '\n' || c == '\r') {
                buf[i] = '\0';
                write(1, "\n", 1);
                break;
            } else if (c == 127 || c == 8) {  /* backspace/delete */
                if (i > 0) {
                    i--;
                    write(1, "\b \b", 3);
                }
            } else if (c == 27) {  /* ESC - exit */
                write(1, "exiting...\n", 12);
                return 0;
            } else {
                if (i < TERM_BUF - 1) {
                    buf[i++] = c;
                    write(1, &c, 1);
                }
            }
        }

        /* Echo back the line */
        if (i > 0) {
            write(1, buf, i);
            write(1, "\n", 1);
        }
    }

    return 0;
}