/* shell_script.c — helixsh script files (`.sh`).
 *
 * Why: helixsh could only run one typed line at a time. A script is the
 * cheapest way to batch work (set up a directory, copy files, print a report),
 * and it is the piece that makes `if`/`then` worth having.
 *
 * What this supports, deliberately small:
 *   - one command per line, through the ordinary pipeline (pipes, >, >>, <)
 *   - `# comment` lines and blank lines
 *   - `$NAME` and `${NAME}` expansion (shell.c), set by `export NAME=VALUE`
 *   - `if <command> [then]` / `else` / `fi`, tested on the command's status
 *   - `test` (and its `[ ... ]` spelling) for file and string conditions
 *
 * Ceiling: no loops, no functions, no quoting, no `elif`, no `$?`. Each of
 * those adds a parser pass over the line; add them when a script in this tree
 * actually needs one. Trailing `# ...` on a command line is not stripped
 * either, only a whole-line comment.
 *
 * Build: picked up automatically by the kernel Makefile's source glob
 * (kernel src directory, .c files).
 */
#include "shell.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "commands.h"
#include "console.h"
#include "ext4.h"
#include "heap.h"
#include "klog.h"
#include "kstring.h"

#define SCRIPT_LINE_MAX 256
#define SCRIPT_DEPTH    16
#define SCRIPT_FILE_MAX (256u * 1024u)

/* ------------------------------------------------------------------ */
/* export / unset                                                      */
/* ------------------------------------------------------------------ */

/* export NAME=VALUE [NAME=VALUE ...]. No arguments lists what is set. */
void cmd_export(int argc, char **argv) {
    if (argc < 2) {
        shell_vars_print();
        return;
    }
    for (int i = 1; i < argc; i++) {
        const char *eq = strchr(argv[i], '=');
        if (eq == NULL) {
            const char *value = shell_var_get(argv[i]);
            console_write("export: ");
            console_write(argv[i]);
            if (value == NULL) {
                console_write(" is not set\n");
            } else {
                console_putc('=');
                console_write(value);
                console_putc('\n');
            }
            continue;
        }
        char name[32];
        size_t n = (size_t)(eq - argv[i]);
        if (n == 0 || n >= sizeof(name)) {
            console_write("export: bad name: ");
            console_write(argv[i]);
            console_putc('\n');
            continue;
        }
        memcpy(name, argv[i], n);
        name[n] = '\0';
        if (!shell_var_set(name, eq + 1)) {
            console_write("export: variable table full (32 entries)\n");
        }
    }
}

/* unset NAME [NAME ...] */
void cmd_unset(int argc, char **argv) {
    if (argc < 2) {
        console_write("usage: unset NAME [NAME ...]\n");
        return;
    }
    for (int i = 1; i < argc; i++) shell_var_unset(argv[i]);
}

/* ------------------------------------------------------------------ */
/* test                                                                */
/* ------------------------------------------------------------------ */

static bool parse_int32(const char *s, int32_t *out) {
    if (s == NULL || *s == '\0') return false;
    size_t i = 0;
    bool neg = false;
    if (s[0] == '-' || s[0] == '+') {
        neg = (s[0] == '-');
        i = 1;
    }
    if (s[i] == '\0') return false;
    int32_t value = 0;
    for (; s[i] != '\0'; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        value = value * 10 + (s[i] - '0');
    }
    *out = neg ? -value : value;
    return true;
}

/* True when the path exists on the mounted root filesystem. */
static bool path_status(const char *path, ext4_stat_t *out_st, bool *is_dir, bool *is_reg) {
    ext4_fs_t *fs = ext4_get_current();
    if (fs == NULL) return false;
    ext4_stat_t st;
    uint64_t ino = shell_resolve_path(fs, path, &st);
    if (!ino || !ext4_stat(fs, ino, &st)) return false;
    if (out_st) *out_st = st;
    if (is_dir) *is_dir = EXT4_S_ISDIR(st.mode) != 0;
    if (is_reg) *is_reg = EXT4_S_ISREG(st.mode) != 0;
    return true;
}

/* test CONDITION, or [ CONDITION ] — the condition `if` runs.
 *   test -f FILE   test -d DIR    test -e PATH   (exists)
 *   test -z STR    test -n STR    test STR
 *   test A = B     test A != B    test A -eq B   (also -ne -lt -gt -le -ge) */
void cmd_test(int argc, char **argv) {
    int first = 1;
    int last = argc;  /* exclusive: argv[last] is the operand past the end */

    if (argc > 0 && strcmp(argv[0], "[") == 0) {
        if (argc < 2 || argv[argc - 1] == NULL || strcmp(argv[argc - 1], "]") != 0) {
            console_write("test: missing ']'\n");
            shell_set_status(2);
            return;
        }
        last = argc - 1;
    }

    int n = last - first;
    char **a = argv + first;

    if (n <= 0) {
        shell_set_status(1);  /* no expression: false */
        return;
    }

    if (n == 1) {
        shell_set_status(a[0][0] != '\0' ? 0 : 1);  /* non-empty string? */
        return;
    }

    if (n == 2 && a[0][0] == '-' && a[0][1] != '\0' && a[0][2] == '\0') {
        const char *op = a[0];
        const char *val = a[1];
        if (strcmp(op, "-z") == 0) { shell_set_status(val[0] == '\0' ? 0 : 1); return; }
        if (strcmp(op, "-n") == 0) { shell_set_status(val[0] != '\0' ? 0 : 1); return; }
        if (strcmp(op, "-e") == 0 || strcmp(op, "-f") == 0 || strcmp(op, "-d") == 0) {
            bool is_dir = false, is_reg = false;
            bool exists = path_status(val, NULL, &is_dir, &is_reg);
            int status = 1;
            if (exists) {
                if (strcmp(op, "-e") == 0) status = 0;
                else if (strcmp(op, "-d") == 0) status = is_dir ? 0 : 1;
                else status = is_reg ? 0 : 1;
            }
            shell_set_status(status);
            return;
        }
        if (strcmp(op, "-r") == 0 || strcmp(op, "-w") == 0 || strcmp(op, "-x") == 0) {
            /* ponytail: permissions are not consulted yet — Helix's ext4 layer
             * has no access() check, so these answer "the path exists". */
            shell_set_status(path_status(val, NULL, NULL, NULL) ? 0 : 1);
            return;
        }
        console_write("test: unknown unary operator: ");
        console_write(op);
        console_putc('\n');
        shell_set_status(2);
        return;
    }

    if (n == 3) {
        const char *lhs = a[0];
        const char *op = a[1];
        const char *rhs = a[2];

        if (strcmp(op, "=") == 0 || strcmp(op, "==") == 0) {
            shell_set_status(strcmp(lhs, rhs) == 0 ? 0 : 1);
            return;
        }
        if (strcmp(op, "!=") == 0) {
            shell_set_status(strcmp(lhs, rhs) == 0 ? 1 : 0);
            return;
        }

        static const char *int_ops[] = {"-eq", "-ne", "-lt", "-gt", "-le", "-ge"};
        for (size_t k = 0; k < sizeof(int_ops) / sizeof(int_ops[0]); k++) {
            if (strcmp(op, int_ops[k]) != 0) continue;
            int32_t x = 0, y = 0;
            if (!parse_int32(lhs, &x) || !parse_int32(rhs, &y)) {
                console_write("test: integer expression expected\n");
                shell_set_status(2);
                return;
            }
            int r;
            if (strcmp(op, "-eq") == 0) r = x == y;
            else if (strcmp(op, "-ne") == 0) r = x != y;
            else if (strcmp(op, "-lt") == 0) r = x < y;
            else if (strcmp(op, "-gt") == 0) r = x > y;
            else if (strcmp(op, "-le") == 0) r = x <= y;
            else r = x >= y;
            shell_set_status(r ? 0 : 1);
            return;
        }
    }

    console_write("usage: test [-f|-d|-e|-z|-n] ARG | test A = B | test A -eq B\n");
    shell_set_status(2);
}

/* ------------------------------------------------------------------ */
/* The script runner                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    bool skipping;    /* lines in this frame are not executed */
    bool took_then;    /* did the then-branch run? drives `else` */
} script_frame_t;

static const char *trim_left(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

static bool word_is(const char *line, const char *word) {
    size_t n = strlen(word);
    if (strncmp(line, word, n) != 0) return false;
    return line[n] == '\0' || line[n] == ' ' || line[n] == '\t';
}

/* Strip a trailing `then` keyword: `if test -f /x then` == `if test -f /x`. */
static void strip_then(char *s) {
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t')) s[--len] = '\0';
    if (len < 4) return;
    if (strncmp(s + len - 4, "then", 4) != 0) return;
    if (len != 4 && s[len - 5] != ' ' && s[len - 5] != '\t') return;
    len -= 4;
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t')) s[--len] = '\0';
}

/* Run one script line. Returns false when the script must stop (bad `fi`). */
static bool script_line(script_frame_t *stack, int *depth, char *text, int *status) {
    const char *line = trim_left(text);
    if (line[0] == '\0' || line[0] == '#') return true;

    bool parent_skip = (*depth > 0) ? stack[*depth - 1].skipping : false;

    if (word_is(line, "if")) {
        if (*depth >= SCRIPT_DEPTH) {
            console_write("sh: if nesting too deep\n");
            return false;
        }
        char cond[SCRIPT_LINE_MAX];
        size_t n = 0;
        const char *src = trim_left(line + 2);  /* past "if" */
        while (*src != '\0' && n + 1 < sizeof(cond)) cond[n++] = *src++;
        cond[n] = '\0';
        strip_then(cond);

        script_frame_t *f = &stack[(*depth)++];
        if (parent_skip) {
            /* Inside a branch that is not running: never evaluate, stay quiet. */
            f->skipping = true;
            f->took_then = false;
            return true;
        }
        if (cond[0] == '\0') {
            console_write("sh: if needs a command\n");
            (*depth)--;
            return false;
        }
        int cond_status = shell_exec_line(cond);
        f->took_then = (cond_status == 0);
        f->skipping = (cond_status != 0);
        return true;
    }

    if (word_is(line, "else")) {
        if (*depth == 0) {
            console_write("sh: else without if\n");
            return false;
        }
        script_frame_t *f = &stack[*depth - 1];
        f->skipping = !f->took_then;
        return true;
    }

    if (word_is(line, "fi")) {
        if (*depth == 0) {
            console_write("sh: fi without if\n");
            return false;
        }
        (*depth)--;
        return true;
    }

    /* A regular command: run it unless we are inside a branch that was skipped. */
    if (parent_skip) return true;
    if (*depth > 0 && stack[*depth - 1].skipping) return true;

    *status = shell_exec_line((char *)line);
    return true;
}

/* Run `path` as a helixsh script. Returns the script's exit status, or a
 * negative value when the file could not be read. */
static int script_run(const char *path) {
    ext4_fs_t *fs = ext4_get_current();
    if (fs == NULL) {
        console_write("sh: no filesystem mounted\n");
        return -1;
    }
    ext4_stat_t st;
    uint64_t ino = shell_resolve_path(fs, path, &st);
    if (!ino || !ext4_stat(fs, ino, &st)) {
        console_write("sh: ");
        console_write(path);
        console_write(": No such file\n");
        return -1;
    }
    if (EXT4_S_ISDIR(st.mode)) {
        console_write("sh: ");
        console_write(path);
        console_write(": Is a directory\n");
        return -1;
    }
    if (st.size > SCRIPT_FILE_MAX) {
        console_write("sh: script too large\n");
        return -1;
    }
    if (st.size == 0) {
        shell_set_status(0);
        return 0;
    }

    char *buf = (char *)kmalloc(st.size + 1);
    if (buf == NULL) {
        console_write("sh: out of memory\n");
        return -1;
    }
    uint64_t got = ext4_read(fs, ino, 0, buf, st.size);
    buf[got] = '\0';

    script_frame_t stack[SCRIPT_DEPTH];
    int depth = 0;
    int status = 0;

    char *cursor = buf;
    bool ok = true;
    while (ok && *cursor != '\0') {
        char *eol = strchr(cursor, '\n');
        size_t len = (eol != NULL) ? (size_t)(eol - cursor) : strlen(cursor);

        char line[SCRIPT_LINE_MAX];
        if (len >= sizeof(line)) {
            console_write("sh: line too long, stopping\n");
            ok = false;
        } else {
            memcpy(line, cursor, len);
            line[len] = '\0';
            if (len > 0 && line[len - 1] == '\r') line[len - 1] = '\0';
            klog_event(KLOG_SHELL, "sh: %s", line);
            ok = script_line(stack, &depth, line, &status);
        }

        if (eol == NULL) break;
        cursor = eol + 1;
    }

    if (ok && depth != 0) {
        console_write("sh: unterminated if (missing fi)\n");
        ok = false;
    }

    kfree(buf);
    shell_set_status(ok ? status : 1);
    return ok ? status : 1;
}

/* sh <file> — run a helixsh script. */
void cmd_sh(int argc, char **argv) {
    if (argc < 2) {
        console_write("usage: sh <script.sh> [ignored arguments]\n");
        shell_set_status(2);
        return;
    }
    /* ponytail: a script gets no $1/$@ of its own; pass argv[2..] as $0-style
     * variables instead by exporting them before the call. */
    int status = script_run(argv[1]);
    shell_set_status(status < 0 ? 1 : status);
}
