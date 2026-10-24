/* commands.h — the built-in command registry.
 *
 * Every shell command is one row in a table. Adding a program is a matter of
 * writing a function with the command signature and adding a row. Later
 * milestones will load real programs from disk instead; for now these are the
 * first Helix "programs", living in the kernel.
 */
#ifndef HELIX_COMMANDS_H
#define HELIX_COMMANDS_H

#include <stdbool.h>
#include <stddef.h>

typedef void (*command_fn)(int argc, char **argv);

typedef struct {
    const char *name;
    const char *help;
    command_fn fn;
} command_t;

extern const command_t helix_commands[];
extern const size_t helix_command_count;

/* Run the boot-provided shell as the initial user process, if present. */
bool commands_start_init(void);

#endif /* HELIX_COMMANDS_H */
