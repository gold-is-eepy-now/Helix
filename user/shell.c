/* shell.c — first interactive user-space command loop. */
#include "helix.h"

static helix_size_t string_length(const char *s) {
    helix_size_t n = 0;
    while (s[n] != '\0') n++;
    return n;
}
static int string_equal(const char *a, const char *b) {
    while (*a != '\0' && *a == *b) { a++; b++; }
    return *a == *b;
}
static void print(const char *s) { (void)helix_write(s, string_length(s)); }
static void print_line(const char *s) { print(s); print("\n"); }

void _start(void) {
    char line[128];
    for (;;) {
        print("user@helix:~$ ");
        long count = helix_read(line, sizeof(line) - 1);
        if (count <= 0) { print_line("input error"); continue; }
        if (line[count - 1] == '\n') count--;
        line[count] = '\0';
        if (string_equal(line, "help")) {
            print_line("commands: help, echo, version, clear, exit");
        } else if (string_equal(line, "version")) {
            print_line("Helix OS 0.9.0-dev.20261018 (user shell)");
        } else if (string_equal(line, "clear")) {
            (void)helix_clear();
        } else if (string_equal(line, "exit")) {
            helix_exit(0);
        } else if (line[0] == 'e' && line[1] == 'c' && line[2] == 'h' &&
                   line[3] == 'o' && (line[4] == ' ' || line[4] == '\0')) {
            print_line(line[4] == ' ' ? line + 5 : "");
        } else if (line[0] != '\0') {
            print_line("command not found (try 'help')");
        }
    }
}
