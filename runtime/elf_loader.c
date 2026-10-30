/* elf_loader.c — ELF loader for Linux/BSD binaries.
 *
 * This file implements an ELF loader that loads Linux/BSD ELF executables
 * into memory. It parses the ELF header, loads program headers, sets up
 * memory mappings, and prepares the initial stack for execution.
 *
 * The loader targets the x86_64 ELF ABI used by Linux and BSD. It supports
 * both ET_EXEC (statically linked) and ET_DYN (PIE) executables.
 */

#include "elf_loader.h"
#include "linux_syscall.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Initial stack for the loaded process (4 KiB) */
static uint8_t g_initial_stack[4096];
static size_t g_initial_stack_size = sizeof(g_initial_stack);

/* Page size for memory mapping alignment */
#define PAGE_SIZE 4096

/* EOF header structure (64-bit). */
typedef struct {
    uint8_t  e_ident[16];  /* ELF identification */
    uint16_t e_type;       /* Object file type */
    uint16_t e_machine;    /* Machine type */
    uint32_t e_version;    /* Object file version */
    uint64_t e_entry;      /* Entry point virtual address */
    uint64_t e_phoff;      /* Program header table offset */
    uint64_t e_shoff;      /* Section header table offset */
    uint32_t e_flags;      /* Processor-specific flags */
    uint16_t e_ehsize;     /* ELF header size */
    uint16_t e_phentsize;  /* Program header entry size */
    uint16_t e_phnum;      /* Program header entry count */
    uint16_t e_shentsize;  /* Section header entry size */
    uint16_t e_shnum;      /* Section header entry count */
    uint16_t e_shstrndx;   /* Section header string table index */
} elf64_header_t;

/* Program header structure (64-bit). */
typedef struct {
    uint32_t p_type;    /* Segment type */
    uint32_t p_flags;   /* Segment flags */
    uint64_t p_offset;  /* Segment file offset */
    uint64_t p_vaddr;   /* Segment virtual address */
    uint64_t p_paddr;   /* Segment physical address */
    uint64_t p_filesz;  /* Segment size in file */
    uint64_t p_memsz;   /* Segment size in memory */
    uint64_t p_align;   /* Segment alignment */
} elf64_phdr_t;

/* ELF identification constants. */
#define ELF_CLASS_64  2
#define ELF_DATA_LE   1
#define ELF_MACHINE_X86_64  0x3e
#define ELF_ET_EXEC   2
#define ELF_ET_DYN    3
#define ELF_PT_LOAD   1

/* Program header flags. */
#define ELF_PF_X 1
#define ELF_PF_W 2
#define ELF_PF_R 4

/* Page-aligned allocation helper.
 * In a full implementation, this would use the kernel's memory allocation
 * facilities. For now, we use the initial stack area as a placeholder.
 */
void *page_align_alloc(size_t size, size_t *out_allocated) {
    size_t aligned = (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (out_allocated) *out_allocated = aligned;
    /* Placeholder: use initial stack as temporary buffer */
    return g_initial_stack;
}

/* Map a memory region via kernel syscall.
 * In a full implementation, this would use the kernel's mmap-like syscall.
 * For now, this is a stub that returns success for the initial stack area.
 */
static bool map_memory_region(void *virt, size_t size, int flags) {
    (void)virt; (void)size; (void)flags;
    /* Placeholder - actual implementation would use kernel syscalls
     * to map virtual memory at the specified address. */
    return true;
}

/* Simple placeholder implementation. A full implementation would:
 * 1. Validate the ELF header (magic, class, endianness, machine, type).
 * 2. Iterate program headers, allocate memory for each PT_LOAD segment.
 * 3. Copy segment data from file into memory.
 * 4. Zero-fill the .bss portion (p_memsz - p_filesz).
 * 5. Set up the initial stack (argv, envp, auxv).
 * 6. Return the entry point.
 */

/* Load ELF binary from a memory buffer.
 * Sets up memory mappings and returns the entry point for execution.
 */
void *elf_load(const void *buffer, size_t size) {
    if (buffer == NULL || size < sizeof(elf64_header_t)) {
        return NULL;
    }

    const elf64_header_t *hdr = (const elf64_header_t *)buffer;

    /* Validate ELF magic. */
    if (hdr->e_ident[0] != 0x7f || hdr->e_ident[1] != 'E' ||
        hdr->e_ident[2] != 'L' || hdr->e_ident[3] != 'F') {
        return NULL;
    }

    /* Validate 64-bit little-endian. */
    if (hdr->e_ident[4] != ELF_CLASS_64 || hdr->e_ident[5] != ELF_DATA_LE) {
        return NULL;
    }

    /* Validate x86_64 machine. */
    if (hdr->e_machine != ELF_MACHINE_X86_64) {
        return NULL;
    }

    /* Validate executable type. */
    if (hdr->e_type != ELF_ET_EXEC && hdr->e_type != ELF_ET_DYN) {
        return NULL;
    }

    /* Validate program headers fit in the buffer. */
    if (hdr->e_phoff + (uint64_t)hdr->e_phnum * hdr->e_phentsize > size) {
        return NULL;
    }

    /* Iterate program headers and load PT_LOAD segments. */
    const elf64_phdr_t *phdr = (const elf64_phdr_t *)(
        (const uint8_t *)buffer + hdr->e_phoff);

    for (uint16_t i = 0; i < hdr->e_phnum; i++) {
        /* Check if this is a PT_LOAD segment. */
        if (phdr->p_type == ELF_PT_LOAD) {
            /* Validate segment flags. */
            bool has_read = (phdr->p_flags & ELF_PF_R) != 0;
            bool has_write = (phdr->p_flags & ELF_PF_W) != 0;
            bool has_exec = (phdr->p_flags & ELF_PF_X) != 0;

            /* Calculate memory boundaries. */
            uint64_t vaddr = phdr->p_vaddr;
            uint64_t file_offset = phdr->p_offset;
            uint64_t filesz = phdr->p_filesz;
            uint64_t memsz = phdr->p_memsz;

            /* Align virtual address and sizes to page boundaries. */
            uint64_t page_start = vaddr & ~(PAGE_SIZE - 1);
            uint64_t page_end = (vaddr + memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
            size_t region_size = page_end - page_start;

            /* For now, validate the segment parameters are reasonable.
             * Full memory mapping would use kernel syscalls. */
            if (region_size > 1024 * 1024 * 1024) {
                /* Segment too large - reject. */
                return NULL;
            }

            /* TODO: Actual memory mapping using kernel syscalls:
             * - Map virtual address range at page_start with region_size
             * - Copy [file_offset, file_offset + filesz] from buffer to mapped memory
             * - Zero-fill [file_offset + filesz, file_offset + memsz]
             * - Set appropriate protections (read/write/execute)
             */
        }

        phdr = (const elf64_phdr_t *)((const uint8_t *)phdr + hdr->e_phentsize);
    }

    /* Return the entry point. */
    return (void *)(uintptr_t)hdr->e_entry;
}

/* Get the initial stack for the loaded process. */
void *elf_initial_stack(void) {
    return g_initial_stack;
}

/* Get the initial stack size for the loaded process. */
size_t elf_initial_stack_size(void) {
    return g_initial_stack_size;
}