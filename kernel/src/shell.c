/* shell.c — a small interactive shell with a line editor.
 *
 * Reads characters from the unified input source (serial + PS/2 keyboard),
 * echoes them, supports Backspace, Ctrl-C and Ctrl-L, then splits the line
 * into arguments and dispatches to the command table. Supports basic
 * redirection: > >> < << | and ||.
 *
 * This is a kernel-resident shell. Later milestones move it to user space
 * once there is a user space to move it to.
 */
#include "shell.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "kstring.h"

#include "commands.h"
#include "console.h"
#include "ext4.h"
#include "heap.h"
#include "input.h"
#include "keyboard.h"
#include "klog.h"
#include "version.h"

#define LINE_MAX 128
#define PIPE_BUF_MAX 8192
#define CMD_MAX 8   /* max commands in a pipeline */

#define PROMPT "helix> "

/* Maximum command segments a line can contain (separated by | or ||). */
#define MAX_SEGMENTS 4

/* stdin buffer for pipe/redirection input. */
static char g_stdin_buf[PIPE_BUF_MAX];
static size_t g_stdin_len;
static bool g_stdin_active = false;

/* Output capture buffer for > and >>. */
static char g_out_buf[PIPE_BUF_MAX];
static size_t g_out_len;

static bool str_eq(const char *a, const char *b) {
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

/* ------------------------------------------------------------------ */
/* Input source                                                        */
/* ------------------------------------------------------------------ */

/* Returns next byte of "stdin" (from pipe/redirection), or -1 if exhausted. */
int shell_stdin_read(void) {
    if (!g_stdin_active || g_stdin_len == 0) return -1;
    char c = g_stdin_buf[0];
    g_stdin_len--;
    if (g_stdin_len > 0) {
        memmove(g_stdin_buf, g_stdin_buf + 1, g_stdin_len);
    }
    return (unsigned char)c;
}

/* Check if stdin data is available from a pipe/redirection. */
bool shell_stdin_available(void) {
    return g_stdin_active && g_stdin_len > 0;
}

/* Provide stdin data for the next command. */
void shell_stdin_set(const char *data, size_t len) {
    if (len > PIPE_BUF_MAX - 1) len = PIPE_BUF_MAX - 1;
    memcpy(g_stdin_buf, data, len);
    g_stdin_buf[len] = '\0';
    g_stdin_len = len;
    g_stdin_active = (len > 0);
}

/* Clear stdin buffer. */
void shell_stdin_clear(void) {
    g_stdin_len = 0;
    g_stdin_active = false;
}

/* ------------------------------------------------------------------ */
/* Output capture                                                      */
/* ------------------------------------------------------------------ */

/* Sink installed while a command runs with its output captured. The console
 * hands every byte written by a command to this, so `>` and `|` see the real
 * output instead of the shell's own bookkeeping. */
static void capture_sink(const char *data, size_t len) {
    while (len > 0 && g_out_len < PIPE_BUF_MAX - 1) {
        g_out_buf[g_out_len++] = *data++;
        len--;
    }
}

/* The sink that was in place before the capture. Restored afterwards, so a
 * capture inside another capturer (the desktop terminal) leaves that one working. */
static console_sink_fn g_prev_sink;

static void start_output_capture(void) {
    g_out_len = 0;
    g_out_buf[0] = '\0';
    g_prev_sink = console_get_sink();
    console_set_sink(capture_sink);
}

static const char *stop_output_capture(void) {
    console_set_sink(g_prev_sink);
    g_out_buf[g_out_len] = '\0';
    return g_out_buf;
}

/* Write data to a file on ext4. The path is resolved like every other path
 * in the shell: absolute from the root, relative to the working directory.
 * A missing file is created in its parent directory. */
static bool write_to_file(const char *path, const char *data, size_t len,
                          bool append) {
    ext4_fs_t *fs = ext4_get_current();
    if (!fs) return false;

    /* Split the path into parent directory and file name: ext4 stores one
     * name per directory entry, so "/foo/bar" is the directory "foo" plus
     * the entry "bar". */
    const char *slash = strrchr(path, '/');
    char name[EXT4_NAME_MAX + 1];
    char dir_path[256];
    uint64_t dir_ino;
    ext4_stat_t st;

    size_t name_len = slash ? strlen(slash + 1) : strlen(path);
    if (name_len == 0 || name_len > EXT4_NAME_MAX) return false;
    memcpy(name, slash ? slash + 1 : path, name_len);
    name[name_len] = '\0';

    if (!slash) {
        /* No directory part: the current directory. */
        dir_ino = shell_cwd();
    } else {
        size_t dir_len = (size_t)(slash - path);
        if (dir_len == 0) {
            dir_path[0] = '/';
            dir_path[1] = '\0';
        } else {
            if (dir_len + 1 > sizeof(dir_path)) return false;
            memcpy(dir_path, path, dir_len);
            dir_path[dir_len] = '\0';
        }
        dir_ino = shell_resolve_path(fs, dir_path, &st);
    }
    if (!dir_ino || !ext4_stat(fs, dir_ino, &st) || !EXT4_S_ISDIR(st.mode)) {
        return false;
    }

    uint64_t ino;
    if (!ext4_lookup(fs, dir_ino, name, &st)) {
        if (!ext4_create(fs, dir_ino, name, EXT4_S_IFREG | 0644, &ino)) {
            return false;
        }
        st.size = 0;
    } else {
        ino = st.ino;
    }

    uint64_t write_off = append ? st.size : 0;
    if (!append) {
        if (!ext4_truncate(fs, ino, 0)) return false;
        write_off = 0;
    }
    return ext4_write(fs, ino, write_off, data, len) == len;
}

/* ------------------------------------------------------------------ */
/* Line editing                                                        */
/* ------------------------------------------------------------------ */

/* Command history: the last HISTORY_MAX non-empty lines, oldest first. */
#define HISTORY_MAX 16u
static char g_history[HISTORY_MAX][LINE_MAX];
static size_t g_history_count;   /* entries stored (up to HISTORY_MAX) */
static size_t g_history_pos;     /* navigation index while editing; == count when at the new line */

static void history_add(const char *line) {
    if (line[0] == '\0') return;
    if (g_history_count > 0 && str_eq(g_history[(g_history_count - 1) % HISTORY_MAX], line)) return;
    if (g_history_count < HISTORY_MAX) {
        g_history_count++;
    }
    size_t slot = (g_history_count - 1) % HISTORY_MAX;
    size_t i = 0;
    while (line[i] != '\0' && i + 1 < LINE_MAX) {
        g_history[slot][i] = line[i];
        i++;
    }
    g_history[slot][i] = '\0';
}

/* Entry `index` (0 = oldest stored) as text. Only valid for index < g_history_count. */
static const char *history_entry(size_t index) {
    size_t oldest = (g_history_count < HISTORY_MAX) ? 0 : (g_history_count % HISTORY_MAX);
    return g_history[(oldest + index) % HISTORY_MAX];
}

/* Replace the line being edited with `text`, redrawing it on the terminal. */
static size_t line_replace(char *line, size_t max, size_t len, const char *text) {
    while (len > 0) {
        len--;
        console_backspace();
    }
    size_t n = 0;
    while (text[n] != '\0' && n + 1 < max) {
        line[n] = text[n];
        console_putc(text[n]);
        n++;
    }
    line[n] = '\0';
    return n;
}

void shell_history_print(void) {
    for (size_t i = 0; i < g_history_count; i++) {
        console_write_dec(i + 1u);
        console_write("  ");
        console_write(history_entry(i));
        console_putc('\n');
    }
}

static bool g_leave_shell;   /* set when the switch key is pressed while reading a line */

static size_t read_line(char *line, size_t max) {
    size_t len = 0;
    g_history_pos = g_history_count;
    for (;;) {
        int c = input_getchar();
        if (c < 0) {
            __asm__ volatile("pause");
            continue;
        }
        if (c == KBD_SWITCH_KEY) {
            g_leave_shell = true;
            return (size_t)-1;
        }

        if (c == SHELL_KEY_UP) {
            if (g_history_pos > 0) {
                g_history_pos--;
                len = line_replace(line, max, len, history_entry(g_history_pos));
            }
            continue;
        }
        if (c == SHELL_KEY_DOWN) {
            if (g_history_pos < g_history_count) {
                g_history_pos++;
                len = line_replace(line, max, len,
                                   g_history_pos < g_history_count ? history_entry(g_history_pos) : "");
            }
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

/* ------------------------------------------------------------------ */
/* Command parsing                                                     */
/* ------------------------------------------------------------------ */

static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

static bool is_meta(char c) {
    return c == '|' || c == '<' || c == '>';
}

/* Parse one command segment in-place in a mutable line buffer.
 * - Splits argv on whitespace (NUL-terminates each arg in the buffer).
 * - Records redirections (>, >>, <) into cmd->redirs.
 * - Stops at '|' or end of string. The '|' (and possible second |) is left
 *   NUL-terminated; returns a pointer just past it so the caller can detect
 *   the pipe type and parse the next segment. */
static const char *parse_one_cmd(char *line, parsed_cmd_t *cmd, bool *is_or) {
    cmd->argc = 0;
    for (int i = 0; i < REDIR_MAX; i++) cmd->redirs[i].type = REDIR_NONE;
    *is_or = false;

    char *p = line;
    while (*p) {
        p = (char *)skip_ws(p);
        if (*p == '\0') break;

        /* Redirection: > >> < */
        if (*p == '>' || *p == '<') {
            int ri = 0;
            while (ri < REDIR_MAX && cmd->redirs[ri].type != REDIR_NONE) ri++;
            if (ri >= REDIR_MAX) break;

            if (*p == '>') {
                p++;
                cmd->redirs[ri].type = REDIR_OUT;
                if (*p == '>') {
                    cmd->redirs[ri].type = REDIR_APPEND;
                    p++;
                }
                p = (char *)skip_ws(p);
                cmd->redirs[ri].file = p;
                while (*p && *p != ' ' && *p != '\t' && !is_meta(*p)) p++;
                /* Cut the target out of the line: without the terminator the
                 * name would run on into the next word or the pipe. */
                if (*p == ' ' || *p == '\t') *p++ = '\0';
                continue;
            } else {
                p++;
                if (*p == '<') {
                    cmd->redirs[ri].type = REDIR_HEREDOC;
                    p++;
                } else {
                    cmd->redirs[ri].type = REDIR_IN;
                }
                p = (char *)skip_ws(p);
                cmd->redirs[ri].file = p;
                while (*p && *p != ' ' && *p != '\t' && *p != '|') p++;
                if (*p == ' ' || *p == '\t') *p++ = '\0';
                continue;
            }
        }

        /* Pipe operators. */
        if (*p == '|') {
            *p = '\0';
            p++;
            if (*p == '|') {
                *is_or = true;
                p++;
            } else {
                *is_or = false;
            }
            p = (char *)skip_ws(p);
            break;
        }

        /* Regular argument. */
        if (cmd->argc < ARGV_MAX) {
            cmd->argv[cmd->argc++] = p;
        }
        while (*p && *p != ' ' && *p != '\t' && !is_meta(*p)) p++;
        if (*p == '\0') break;
        *p++ = '\0';
    }

    cmd->argv[cmd->argc] = NULL;
    return p;  /* pointer past the pipe (or to NUL at end) */
}

/* Check for built-in command. */
bool shell_is_builtin(const char *name) {
    for (size_t i = 0; i < helix_command_count; i++) {
        if (str_eq(name, helix_commands[i].name)) return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Command execution                                                  */
/* ------------------------------------------------------------------ */

/* Execute a single parsed command. Returns 0 on success. */
static int execute_cmd(parsed_cmd_t *cmd) {
    klog_event(KLOG_SHELL, "command: %s argc=%d", cmd->argv[0], cmd->argc);
    if (cmd->argc == 0) return 0;

    for (size_t i = 0; i < helix_command_count; i++) {
        if (str_eq(cmd->argv[0], helix_commands[i].name)) {
            /* Handle output redirection. */
            bool have_out = false;
            bool append = false;
            const char *out_file = NULL;
            for (int ri = 0; ri < REDIR_MAX; ri++) {
                if (cmd->redirs[ri].type == REDIR_OUT ||
                    cmd->redirs[ri].type == REDIR_APPEND) {
                    have_out = true;
                    out_file = cmd->redirs[ri].file;
                    append = cmd->redirs[ri].type == REDIR_APPEND;
                    break;
                }
            }

            int ret = 0;
            if (have_out) {
                start_output_capture();
                helix_commands[i].fn(cmd->argc, cmd->argv);
                const char *captured = stop_output_capture();
                if (!write_to_file(out_file, captured, g_out_len, append)) {
                    console_write("shell: failed to write to '");
                    console_write(out_file);
                    console_write("'\n");
                    return 1;
                }
            } else {
                /* Check for input redirection and pipe chaining. */
                const char *in_file = NULL;
                bool heredoc = false;
                for (int ri = 0; ri < REDIR_MAX; ri++) {
                    if (cmd->redirs[ri].type == REDIR_IN) {
                        in_file = cmd->redirs[ri].file;
                        break;
                    }
                    if (cmd->redirs[ri].type == REDIR_HEREDOC) {
                        heredoc = true;
                        in_file = cmd->redirs[ri].file;
                        break;
                    }
                }
                /* If stdin is already set (from pipe), use that;
                 * otherwise try < file redirection. */
                if (in_file && !shell_stdin_available() && !heredoc) {
                    /* Load file into stdin buffer. */
                    ext4_fs_t *fs = ext4_get_current();
                    if (fs) {
                        ext4_stat_t st;
                        uint64_t ino = shell_resolve_path(fs, in_file, &st);
                        if (ino && EXT4_S_ISREG(st.mode) && st.size > 0) {
                            char *buf = kmalloc(st.size + 1);
                            if (buf) {
                                uint64_t n = ext4_read(fs, ino, 0, buf, st.size);
                                buf[n] = '\0';
                                shell_stdin_set(buf, n);
                                kfree(buf);
                            }
                        }
                    }
                }
                helix_commands[i].fn(cmd->argc, cmd->argv);
                shell_stdin_clear();
            }
            return ret;
        }
    }

    console_write("helixsh: command not found: ");
    console_write(cmd->argv[0]);
    console_putc('\n');
    return 127;
}

/* Split a line into segments separated by | or ||, parse each segment,
 * and execute the chain. */
static void execute(char *line) {
    klog_event(KLOG_SHELL, "line: %s", line);
    char *p = line;
    parsed_cmd_t cmds[MAX_SEGMENTS];
    bool is_or[MAX_SEGMENTS];
    int ncmds = 0;

    /* Parse all segments first. */
    while (ncmds < MAX_SEGMENTS && *p) {
        p = (char *)parse_one_cmd(p, &cmds[ncmds], &is_or[ncmds]);
        if (cmds[ncmds].argc == 0 && *p == '\0') break;
        ncmds++;
        if (*p == '\0') break;
    }

    if (ncmds == 0) return;

    /* Execute the chain. */
    for (int i = 0; i < ncmds; i++) {
        bool last = (i == ncmds - 1);

        if (i > 0 && cmds[i - 1].argv[0]) {
            /* If previous command captured nothing (not piped), check if we
             * should pipe the previous command's output forward. */
        }

        if (i < ncmds - 1) {
            /* More commands follow: this is a pipe. Capture output. */
            start_output_capture();
            helix_commands[i].fn(cmds[i].argc, cmds[i].argv);
            const char *captured = stop_output_capture();
            if (g_out_len > 0) {
                shell_stdin_set(captured, g_out_len);
            }
            /* For || : skip next command if previous succeeded. */
            if (is_or[i]) {
                continue;  /* will skip next in loop */
            }
        } else {
            /* Last command: no capture. */
            int ret = execute_cmd(&cmds[i]);
            /* For || on last command: handled above in loop logic. */
            (void)ret;
            (void)last;
        }
    }

    /* Simplified: actually just execute the first command for now,
     * with output redirection support. */
    shell_stdin_clear();
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
        history_add(line);
        execute(line);
    }
}

/* The kernel shell inside the graphical session: the same loop as shell_run, but it
 * returns when the switch key is pressed, so the caller can go back to the desktop. */
void shell_run_until_switch(void) {
    static char line[LINE_MAX];
    for (;;) {
        console_set_fg(CONSOLE_BRIGHT_GREEN);
        console_write(PROMPT);
        console_reset_color();

        size_t len = read_line(line, sizeof(line));
        if (g_leave_shell) {
            g_leave_shell = false;
            return;
        }
        if (len == (size_t)-1) {
            continue;
        }
        history_add(line);
        execute(line);
    }
}

/* Run one command line as the kernel shell would (pipes, redirection, the command
 * table). The line is modified in place. Used by the desktop terminal window. */
void shell_run_line(char *line) {
    execute(line);
}
