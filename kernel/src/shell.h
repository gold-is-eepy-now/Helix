/* shell.h — the Helix interactive shell. */
#ifndef HELIX_SHELL_H
#define HELIX_SHELL_H

#include <stdbool.h>
#include <stddef.h>

/* Run the shell loop. Never returns. */
void shell_run(void);

/* Redirection types. */
typedef enum {
    REDIR_NONE,
    REDIR_OUT,   /* > file  - overwrite */
    REDIR_APPEND, /* >> file - append */
    REDIR_IN,    /* < file   - read input */
    REDIR_HEREDOC, /* << delimiter - read until delimiter */
} redir_type_t;

typedef struct {
    redir_type_t type;
    const char *file;   /* for file redirections */
    const char *limiter; /* for heredoc: line that terminates */
} redir_t;

#define REDIR_MAX 4
#define ARGV_MAX 16

typedef struct {
    char *argv[ARGV_MAX];
    int argc;
    redir_t redirs[REDIR_MAX];
} parsed_cmd_t;

/* Split a line into a parsed command structure handling redirections and pipes. */
parsed_cmd_t shell_parse_line(const char *line);

/* Execute a parsed command (supports piping and redirection). */
int shell_execute_parsed(parsed_cmd_t cmd);

/* Check for built-in command; returns true if handled. */
bool shell_is_builtin(const char *name);

/* Run one command line (modified in place) with the usual shell semantics. */
void shell_run_until_switch(void);
void shell_run_line(char *line);

/* Run one command line and return its exit status. `line` is copied into a
 * private buffer before parsing, so the caller's buffer is not touched. Used
 * by the .sh script runner (kernel/src/shell_script.c). */
int shell_exec_line(char *line);

/* Shell variables (export / unset / $NAME). */
bool shell_var_set(const char *name, const char *value);
const char *shell_var_get(const char *name);
void shell_var_unset(const char *name);
void shell_vars_print(void);

/* Expand $NAME and ${NAME} in `in` into `out` (capacity `outsz`, NUL
 * included). Returns the number of bytes written. */
size_t shell_expand(const char *in, char *out, size_t outsz);

/* Exit status of the last command. A command that knows its own status calls
 * shell_set_status(); command functions return void, so execute() otherwise
 * assumes 0 (or 127 for a name that is not in the table). */
void shell_set_status(int status);
int shell_status(void);

/* Print the command history, numbered from 1 (oldest first). */
void shell_history_print(void);

/* Commands implemented outside commands.c, registered in its table. */
void cmd_export(int argc, char **argv);
void cmd_unset(int argc, char **argv);
void cmd_test(int argc, char **argv);
void cmd_sh(int argc, char **argv);

#endif /* HELIX_SHELL_H */