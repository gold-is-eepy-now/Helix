#ifndef HELIX_ELF_LOADER_H
#define HELIX_ELF_LOADER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ELF loader for Linux/BSD binaries.
 *
 * Loads an ELF executable (ET_EXEC, ET_DYN) into memory, sets up the initial
 * process state, and starts execution. This is distinct from the native
 * ELF loader in M9; it targets Linux/BSD ABI compatibility.
 */

/* Load an ELF file from the given buffer into memory. Returns the entry
 * point address on success, or NULL on failure. The buffer must contain
 * the full ELF file in memory (e.g., loaded from the ramdisk). */
void *elf_load(const void *buffer, size_t size);

/* Get the initial stack pointer for the loaded program. Returns the
 * address of the initial stack (argv, envp, auxv) or NULL on failure. */
void *elf_initial_stack(void);

/* Get the initial stack size. */
size_t elf_initial_stack_size(void);

#ifdef __cplusplus
}
#endif

#endif /* HELIX_ELF_LOADER_H */