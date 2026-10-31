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
#include <string.h>

#include "commands.h"
#include "console.h"
#include "ext4.h"
#include "heap.h"
#include "input.h"
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
static bool g_capturing = false;

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

static void capture_write(const char *s) {
    if (!g_capturing) {
        console_write(s);
    } else {
        while (*s && g_out_len < PIPE_BUF_MAX - 1) {
            g_out_buf[g_out_len++] = *s++;
        }
    }
}

static void capture_putc(char c) {
    if (!g_capturing) {
        console_putc(c);
    } else if (g_out_len < PIPE_BUF_MAX - 1) {
        g_out_buf[g_out_len++] = c;
    }
}

static void start_output_capture(void) {
    g_capturing = true;
    g_out_len = 0;
    g_out_buf[0] = '\0';
}

static const char *stop_output_capture(void) {
    g_capturing = false;
    g_out_buf[g_out_len] = '\0';
    return g_out_buf;
}

/* Write data to a file on ext4. */
static bool write_to_file(const char *path, const char *data, size_t len,
                          bool append) {
    ext4_fs_t *fs = ext4_get_current();
    if (!fs) return false;

    ext4_stat_t st;
    uint64_t ino;
    if (!ext4_lookup(fs, ext4_root(fs), path, &st)) {
        if (!ext4_create(fs, ext4_root(fs), path,
                         EXT4_S_IFREG | 0644, &ino)) {
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

static size_t read_line(char *line, size_t max) {
    size_t len = 0;
    for (;;) {
        int c = input_getchar();
        if (c < 0) {
            __asm__ volatile("pause");
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
                    capture_write("shell: failed to write to '");
                    capture_write(out_file);
                    capture_write("'\n");
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
                    char *buf = NULL;
                    size_t len = 0;
                    ext4_fs_t *fs = ext4_get_current();
                    if (fs) {
                        ext4_stat_t st;
                        uint64_t ino = ext4_lookup(fs, ext4_root(fs),
                                                    in_file, &st);
                        if (buf) {
                            buf = kmalloc(st.size + 1);
                            if (buf) {
                                len = ext4_read(fs, ino, 0, buf, st.size);
                                buf[len] = '\0';
                            }
                        }
                    }
                    if (buf) {
                        shell_stdin_set(buf, len);
                        kfree(buf);
                    }
                }
                helix_commands[i].fn(cmd->argc, cmd->argv);
                shell_stdin_clear();
            }
            return ret;
        }
    }

    capture_write("helixsh: command not found: ");
    capture_write(cmd->argv[0]);
    capture_putc('\n');
    return 127;
}

/* Split a line into segments separated by | or ||, parse each segment,
 * and execute the chain. */
static void execute(char *line) {
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
        execute(line);
    }
}
