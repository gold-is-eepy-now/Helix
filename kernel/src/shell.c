/* shell.c — a small interactive shell with a line editor.
 *
 * Reads characters from the unified input source (serial + PS/2 keyboard),
 * echoes them, supports Backspace, Ctrl-C and Ctrl-L, then splits the line
 * into arguments and dispatches to the command table.
 *
 * This is a kernel-resident shell. Later milestones move it to user space
 * once there is a user space to move it to.
 */
#include "shell.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "commands.h"
#include "console.h"
#include "input.h"
#include "version.h"

#define LINE_MAX 128
#define ARGV_MAX 16

#define PROMPT "helix> "

static bool str_eq(const char *a, const char *b) {
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

/* Read one line with basic editing. Returns the length, or (size_t)-1 if the
 * line was cancelled with Ctrl-C. */
static size_t read_line(char *line, size_t max) {
    size_t len = 0;
    for (;;) {
        int c = input_getchar();
        if (c < 0) {
            __asm__ volatile("pause"); /* be gentle while polling */
            continue;
        }

        if (c == '\r' || c == '\n') {
            console_putc('\n');
            line[len] = '\0';
            return len;
        }
        if (c == 0x7F || c == 0x08) { /* Backspace */
            if (len > 0) {
                len--;
                console_backspace();
            }
            continue;
        }
        if (c == 0x03) { /* Ctrl-C */
            console_write("^C\n");
            line[0] = '\0';
            return (size_t)-1;
        }
        if (c == 0x0C) { /* Ctrl-L: redraw */
            console_clear();
            console_write(PROMPT);
            for (size_t i = 0; i < len; i++) {
                console_putc(line[i]);
            }
            continue;
        }
        if (c >= 0x20 && c < 0x7F && len + 1 < max) {
            line[len++] = (char)c;
            console_putc((char)c);
        }
    }
}

/* Split a line into whitespace-separated arguments (in place) and run it. */
static void execute(char *line) {
    char *argv[ARGV_MAX];
    int argc = 0;

    char *p = line;
    while (*p != '\0') {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        if (argc < ARGV_MAX) {
            argv[argc++] = p;
        }
        while (*p != '\0' && *p != ' ' && *p != '\t') {
            p++;
        }
        if (*p != '\0') {
            *p++ = '\0';
        }
    }

    if (argc == 0) {
        return;
    }

    for (size_t i = 0; i < helix_command_count; i++) {
        if (str_eq(argv[0], helix_commands[i].name)) {
            helix_commands[i].fn(argc, argv);
            return;
        }
    }

    console_set_fg(CONSOLE_RED);
    console_write("helixsh: command not found: ");
    console_write(argv[0]);
    console_reset_color();
    console_putc('\n');
}

void shell_run(void) {
    static char line[LINE_MAX];
    for (;;) {
        console_set_fg(CONSOLE_BRIGHT_GREEN);
        console_write(PROMPT);
        console_reset_color();

        size_t len = read_line(line, sizeof(line));
        if (len == (size_t)-1) {
            continue; /* cancelled */
        }
        execute(line);
    }
}
