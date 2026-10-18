/* elf.h — restricted ELF64 loader for native Helix executables. */
#ifndef HELIX_ELF_H
#define HELIX_ELF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "vmm.h"

#define ELF64_MAX_LOAD_PAGES 8192u

typedef struct {
    uint64_t virt;
    uintptr_t phys;
    uint64_t permissions;
} elf64_page_t;

typedef struct {
    uint64_t entry;
    size_t page_count;
    elf64_page_t *pages;
} elf64_image_t;

/* Load a static x86-64 ET_EXEC image into an existing user address space.
 * PT_LOAD segments are supported; dynamic linking and interpreters are not.
 */
bool elf64_load(vmm_address_space_t *space, const void *file, size_t size,
                elf64_image_t *image);
bool elf64_unload(vmm_address_space_t *space, elf64_image_t *image);

#endif /* HELIX_ELF_H */
