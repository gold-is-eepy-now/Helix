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

#endif /* HELIX_SHELL_H */