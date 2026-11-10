/* edit.c: /bin/edit FILE — a line editor for a text file.
 *
 * Type a command and press Enter:
 *   p            print the file with line numbers
 *   a TEXT       add TEXT as a new last line
 *   i N TEXT     insert TEXT before line N
 *   r N TEXT     replace line N with TEXT
 *   d N          delete line N
 *   w            save the file
 *   q            quit (refused while there are unsaved changes; q! quits anyway)
 *   h            help
 *
 * Keys are read by polling SYS_READ_INPUT, the way the userland shell reads them.
 * The file is read and written with the ordinary open/read/write calls. The whole
 * file is held in memory as lines; a line longer than LINE_CAP-1 bytes is cut. */
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include "libhelix.h"

#define MAX_LINES 256
#define LINE_CAP  128

static char (*g_lines)[LINE_CAP];   /* g_count lines, each NUL-terminated */
static int g_count;
static int g_dirty;
static int g_cut;                   /* lines cut short when the file was read */
static const char *g_path;

static uint64_t read_key(void) {
    for (;;) {
        uint64_t r = helix_syscall(SYS_READ_INPUT, 0, 0, 0, 0, 0, 0, 0);
        if ((int64_t)r >= 0) return r;
        __asm__ volatile("pause");
    }
}

static void put_num(int v) {
    char b[12];
    int i = 11;
    b[i] = '\0';
    do {
        b[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v != 0);
    printf("%s", &b[i]);
}

/* Read one line of input, echoing it. Enter ends it; Backspace erases. */
static size_t read_command(char *buf, size_t cap) {
    size_t len = 0;
    for (;;) {
        int c = (int)read_key();
        if (c == '\r' || c == '\n') {
            write(1, "\n", 1);
            buf[len] = '\0';
            return len;
        }
        if (c == 0x7f || c == 0x08) {
            if (len > 0) {
                len--;
                write(1, "\b \b", 3);
            }
            continue;
        }
        if (c >= 0x20 && c <= 0x7e && len + 1 < cap) {
            buf[len++] = (char)c;
            write(1, &buf[len - 1], 1);
        }
    }
}

static int add_line(const char *s, size_t n) {
    if (g_count >= MAX_LINES) return 0;
    size_t k = 0;
    while (k < n && k < LINE_CAP - 1) {
        g_lines[g_count][k] = s[k];
        k++;
    }
    g_lines[g_count][k] = '\0';
    if (n > LINE_CAP - 1) g_cut++;
    g_count++;
    return 1;
}

static void load(void) {
    int fd = open(g_path, O_RDONLY);
    if (fd < 0) {
        printf("new file: %s\n", g_path);
        return;
    }
    char chunk[256];
    char line[LINE_CAP * 2];
    size_t len = 0;
    for (;;) {
        ssize_t n = read(fd, chunk, sizeof chunk);
        if (n <= 0) break;
        for (ssize_t i = 0; i < n; i++) {
            if (chunk[i] == '\n') {
                add_line(line, len);
                len = 0;
            } else if (len < sizeof line - 1) {
                line[len++] = chunk[i];
            }
        }
    }
    if (len > 0) add_line(line, len);   /* a last line with no newline */
    close(fd);
    if (g_cut > 0) printf("note: %d long line(s) were cut to %d bytes\n", g_cut, LINE_CAP - 1);
}

static int save(void) {
    int fd = open(g_path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        printf("could not open %s for writing\n", g_path);
        return 0;
    }
    for (int i = 0; i < g_count; i++) {
        size_t n = 0;
        while (g_lines[i][n] != '\0') n++;
        write(fd, g_lines[i], n);
        write(fd, "\n", 1);
    }
    close(fd);
    g_dirty = 0;
    printf("wrote %d line(s) to %s\n", g_count, g_path);
    return 1;
}

/* Parse a 1-based line number at *p (after spaces). Returns 0 if there is none. */
static int parse_number(const char **p) {
    while (**p == ' ') (*p)++;
    if (**p < '0' || **p > '9') return 0;
    int v = 0;
    while (**p >= '0' && **p <= '9') {
        v = v * 10 + (**p - '0');
        (*p)++;
    }
    return v;
}

static void print_all(void) {
    if (g_count == 0) printf("(empty)\n");
    for (int i = 0; i < g_count; i++) {
        put_num(i + 1);
        printf("  %s\n", g_lines[i]);
    }
}

static void remove_at(int at) {
    for (int i = at; i + 1 < g_count; i++) {
        for (int k = 0; k < LINE_CAP; k++) g_lines[i][k] = g_lines[i + 1][k];
    }
    g_count--;
    g_dirty = 1;
}

static size_t text_len(const char *s) {
    size_t n = 0;
    while (s[n] != '\0') n++;
    return n;
}

/* Run one command line. Returns 1 when the editor should quit. */
static int run(char *cmd) {
    const char *p = cmd;
    while (*p == ' ') p++;
    if (*p == '\0') return 0;
    char op = *p++;

    if (op == 'h') {
        printf("p | a TEXT | i N TEXT | r N TEXT | d N | w | q | q! | h\n");
    } else if (op == 'p') {
        print_all();
    } else if (op == 'a' && (*p == '\0' || *p == ' ')) {
        while (*p == ' ') p++;
        if (add_line(p, text_len(p))) g_dirty = 1;
        else printf("the file is full (%d lines)\n", MAX_LINES);
    } else if (op == 'i' || op == 'r' || op == 'd') {
        int n = parse_number(&p);
        if (n < 1) {
            printf("%c: give a line number, e.g. %c 1\n", op, op);
            return 0;
        }
        if (op == 'd') {
            if (n > g_count) printf("no line %d\n", n);
            else remove_at(n - 1);
            return 0;
        }
        while (*p == ' ') p++;
        if (op == 'r') {
            if (n > g_count) {
                printf("no line %d\n", n);
                return 0;
            }
            size_t k = 0;
            while (p[k] != '\0' && k < LINE_CAP - 1) {
                g_lines[n - 1][k] = p[k];
                k++;
            }
            g_lines[n - 1][k] = '\0';
            g_dirty = 1;
        } else {                                  /* i: insert before line n */
            if (n > g_count + 1) {
                printf("no line %d\n", n);
                return 0;
            }
            if (g_count >= MAX_LINES) {
                printf("the file is full (%d lines)\n", MAX_LINES);
                return 0;
            }
            for (int i = g_count; i > n - 1; i--) {
                for (int k = 0; k < LINE_CAP; k++) g_lines[i][k] = g_lines[i - 1][k];
            }
            g_count++;
            size_t k = 0;
            while (p[k] != '\0' && k < LINE_CAP - 1) {
                g_lines[n - 1][k] = p[k];
                k++;
            }
            g_lines[n - 1][k] = '\0';
            g_dirty = 1;
        }
    } else if (op == 'w') {
        save();
    } else if (op == 'q') {
        if (g_dirty && !(*p == '!')) {
            printf("unsaved changes: w saves, q! quits without saving\n");
            return 0;
        }
        return 1;
    } else {
        printf("unknown command: %s\n", cmd);
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: edit <file>\n");
        return 1;
    }
    g_path = argv[1];
    g_lines = helix_malloc(sizeof(char[LINE_CAP]) * MAX_LINES);
    if (g_lines == NULL) {
        printf("edit: out of memory\n");
        return 1;
    }
    load();
    printf("edit %s: %d line(s). Type h for help.\n", g_path, g_count);
    char cmd[LINE_CAP];
    for (;;) {
        printf("edit> ");
        fflush(stdout);
        read_command(cmd, sizeof cmd);
        if (run(cmd)) break;
    }
    return 0;
}
