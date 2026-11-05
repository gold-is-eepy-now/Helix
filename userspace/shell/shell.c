/* shell.c — the Helix userland shell: a ring-3 program with its own line editor
 * and command history.
 *
 * It runs in its own address space and talks to the kernel only through the
 * native syscalls in libhelix:
 *   SYS_READ_INPUT (fd 0)  poll the console; -1 when no key is waiting;
 *                          arrow keys arrive as 0x101 (up) and 0x102 (down);
 *   SYS_DEBUG_WRITE        write a buffer to the console (temporary syscall
 *                          until a real write path exists);
 *   SYS_EXIT               via helix_exit, when `exit` is typed.
 *
 * Built-ins: echo, cat, write, heap, history, help, exit. Up/Down recall the
 * last 16 lines. */
#include "libhelix.h"

#define SYS_DEBUG_WRITE_NUM 26u
#define KEY_UP   0x101
#define KEY_DOWN 0x102
#define LINE_MAX 128
#define HIST_MAX 16

static char g_hist[HIST_MAX][LINE_MAX];
static size_t g_hist_count;   /* stored lines, at most HIST_MAX */
static size_t g_hist_next;    /* ring slot the next line goes into */

static void out(const char *s, size_t n) {
    helix_syscall(SYS_DEBUG_WRITE_NUM, (uint64_t)(uintptr_t)s, (uint64_t)n, 0, 0, 0, 0, 0);
}

static void say(const char *s) {
    size_t n = 0;
    while (s[n] != '\0') n++;
    out(s, n);
}

static void say_dec(size_t v) {
    char buf[24];
    size_t i = sizeof(buf);
    buf[--i] = '\0';
    do {
        buf[--i] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v != 0 && i > 0);
    say(&buf[i]);
}

/* Block (by polling) until a key arrives. */
static int getkey(void) {
    for (;;) {
        uint64_t r = helix_syscall(SYS_READ_INPUT, 0, 0, 0, 0, 0, 0, 0);
        if ((int64_t)r >= 0) return (int)r;
        __asm__ volatile("pause");
    }
}

/* Whole-string equality. */
static int str_eq(const char *a, const char *b) {
    size_t i = 0;
    while (a[i] != '\0' && a[i] == b[i]) i++;
    return a[i] == b[i];
}

/* True if `word` is the start of `s` (the caller checks what follows it). */
static int starts_with(const char *s, const char *word) {
    size_t i = 0;
    while (word[i] != '\0') {
        if (s[i] != word[i]) return 0;
        i++;
    }
    return 1;
}

/* History entry 0 is the oldest stored line. */
static const char *hist_entry(size_t index) {
    size_t oldest = (g_hist_count < HIST_MAX) ? 0 : g_hist_next;
    return g_hist[(oldest + index) % HIST_MAX];
}

static void hist_add(const char *line, size_t len) {
    if (len == 0) return;
    if (g_hist_count > 0 && str_eq(hist_entry(g_hist_count - 1), line)) {
        return;
    }
    for (size_t i = 0; i <= len; i++) {
        g_hist[g_hist_next][i] = line[i];
    }
    g_hist_next = (g_hist_next + 1) % HIST_MAX;
    if (g_hist_count < HIST_MAX) g_hist_count++;
}

/* Erase the `len` characters on screen and show `text` in their place. */
static size_t redraw(char *line, size_t len, const char *text) {
    for (size_t i = 0; i < len; i++) out("\b \b", 3);
    size_t n = 0;
    while (text[n] != '\0' && n + 1 < LINE_MAX) {
        line[n] = text[n];
        out(&line[n], 1);
        n++;
    }
    line[n] = '\0';
    return n;
}

static size_t read_line(char *line) {
    size_t len = 0;
    size_t pos = g_hist_count;   /* == count means "the new, empty line" */
    for (;;) {
        int c = getkey();
        if (c == '\r' || c == '\n') {
            say("\n");
            line[len] = '\0';
            return len;
        }
        if (c == KEY_UP) {
            if (pos > 0) {
                pos--;
                len = redraw(line, len, hist_entry(pos));
            }
            continue;
        }
        if (c == KEY_DOWN) {
            if (pos < g_hist_count) {
                pos++;
                len = redraw(line, len, pos < g_hist_count ? hist_entry(pos) : "");
            }
            continue;
        }
        if (c == 0x7F || c == '\b') {
            if (len > 0) {
                len--;
                out("\b \b", 3);
            }
            continue;
        }
        if (c >= 0x20 && c <= 0x7E && len + 1 < LINE_MAX) {
            line[len] = (char)c;
            len++;
            out(&line[len - 1], 1);
        }
    }
}

static void cmd_history(void) {
    for (size_t i = 0; i < g_hist_count; i++) {
        say_dec(i + 1u);
        say("  ");
        say(hist_entry(i));
        say("\n");
    }
}

/* cat PATH: print the file's bytes exactly. */
static void cmd_cat(const char *path) {
    int fd = helix_open(path, HELIX_O_RDONLY);
    if (fd < 0) {
        say("cat: cannot open ");
        say(path);
        say("\n");
        return;
    }
    char buf[128];
    int64_t n;
    while ((n = helix_read(fd, buf, sizeof(buf))) > 0) {
        out(buf, (size_t)n);
    }
    helix_close(fd);
}

/* write PATH TEXT: create or replace PATH with TEXT (no newline added). */
static void cmd_write(const char *path, const char *text) {
    int fd = helix_open(path, HELIX_O_WRONLY | HELIX_O_CREAT | HELIX_O_TRUNC);
    if (fd < 0) {
        say("write: cannot open ");
        say(path);
        say("\n");
        return;
    }
    size_t n = 0;
    while (text[n] != '\0') n++;
    int64_t put = helix_write(fd, text, n);
    helix_close(fd);
    say("wrote ");
    say_dec(put < 0 ? 0u : (size_t)put);
    say(" bytes\n");
}

/* heap: allocate 64 KiB on the program break, fill and check it, free it, then
 * allocate a small block and confirm the freed block is reused. */
#define HEAP_TEST_BYTES 65536u
static void cmd_heap(void) {
    unsigned char *p = helix_malloc(HEAP_TEST_BYTES);
    if (p == NULL) {
        say("heap: malloc failed\n");
        return;
    }
    for (size_t i = 0; i < HEAP_TEST_BYTES; i++) p[i] = (unsigned char)(i * 7u);
    int ok = 1;
    for (size_t i = 0; i < HEAP_TEST_BYTES; i++) {
        if (p[i] != (unsigned char)(i * 7u)) { ok = 0; break; }
    }
    helix_free_mem(p);
    unsigned char *q = helix_malloc(1024);
    int reused = q == p;
    say(ok ? "heap: 65536 bytes written and verified" : "heap: FAILED verification");
    say(reused ? ", freed block reused\n" : ", freed block not reused\n");
}

/* Run one non-empty line. Returns 1 if the shell should exit. */
static int run_line(const char *line) {
    if (str_eq(line, "exit")) return 1;
    if (str_eq(line, "history")) {
        cmd_history();
        return 0;
    }
    if (str_eq(line, "help")) {
        say("commands: echo <text>, cat <path>, write <path> <text>,\n");
        say("          heap, history, help, exit\n");
        say("Up/Down recall earlier lines.\n");
        return 0;
    }
    if (str_eq(line, "heap")) {
        cmd_heap();
        return 0;
    }
    if (starts_with(line, "cat") && line[3] == ' ') {
        cmd_cat(line + 4);
        return 0;
    }
    if (starts_with(line, "write") && line[5] == ' ') {
        /* The path is the next word; the text is the rest of the line. */
        char path[128];
        const char *p = line + 6;
        size_t n = 0;
        while (p[n] != '\0' && p[n] != ' ' && n < sizeof(path) - 1) {
            path[n] = p[n];
            n++;
        }
        path[n] = '\0';
        const char *text = p[n] == ' ' ? p + n + 1 : p + n;
        cmd_write(path, text);
        return 0;
    }
    /* `echo` alone prints an empty line; `echo text` prints text. */
    if (starts_with(line, "echo") && (line[4] == '\0' || line[4] == ' ')) {
        const char *rest = line[4] == ' ' ? line + 5 : line + 4;
        say(rest);
        say("\n");
        return 0;
    }
    say("unknown command: ");
    say(line);
    say("\n");
    return 0;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    char line[LINE_MAX];
    say("Helix userland shell (ring 3). Type 'help'.\n");
    for (;;) {
        say("ring3> ");
        size_t len = read_line(line);
        if (len > 0) {
            hist_add(line, len);
            if (run_line(line)) break;
        }
    }
    say("left the userland shell\n");
    return 0;
}
