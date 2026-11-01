/* commands.h — shell command table.
 *
 * Each command is a kernel-resident builtin: compiled into the kernel ELF and
 * executed in kernel context. This is honest for this milestone, as real
 * separately-compiled user-space programs would need the memory manager,
 * ELF loader and syscall ABI of later milestones.
 */

#ifndef COMMANDS_H
#define COMMANDS_H

#include <stddef.h>
#include <stdint.h>

/* Forward declaration for command function type */
typedef void (*command_fn)(int argc, char **argv);

/* Command table entry */
typedef struct {
    const char *name;
    const char *help;
    command_fn fn;
} command_t;

/* Extern declarations for the command table and count (defined in commands.c) */
extern const command_t helix_commands[];
extern const size_t helix_command_count;

#endif /* COMMANDS_H */