/* elf.c — validate and map static ELF64 executable load segments. */
#include "elf.h"

#include <stddef.h>

#include "heap.h"
#include "kstring.h"
#include "pmm.h"
#include "sync.h"

#define ELF_MAGIC 0x464C457Fu
#define ELFCLASS64 2u
#define ELFDATA2LSB 1u
#define ET_EXEC 2u
#define EM_X86_64 62u
#define PT_LOAD 1u
#define PT_DYNAMIC 2u
#define PT_INTERP 3u
#define PF_X 1u
#define PF_W 2u
#define PF_R 4u
#define ELF_USER_TOP 0x0000800000000000ULL

typedef struct __attribute__((packed)) {
    uint8_t ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t phoff;
    uint64_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
} elf64_header_t;

typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t vaddr;
    uint64_t paddr;
    uint64_t filesz;
    uint64_t memsz;
    uint64_t align;
} elf64_phdr_t;

static bool range_in_file(uint64_t offset, uint64_t length, size_t size) {
    return offset <= size && length <= (uint64_t)size - offset;
}

static bool power_of_two(uint64_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

static uint64_t page_floor(uint64_t value) {
    return value & ~((uint64_t)PAGE_SIZE - 1);
}

static bool read_program_header(const uint8_t *file,
                                const elf64_header_t *header,
                                uint16_t index, elf64_phdr_t *out) {
    uint64_t offset = header->phoff + (uint64_t)index * header->phentsize;
    memcpy(out, file + offset, sizeof(*out));
    return true;
}

static bool release_partial(vmm_address_space_t *space,
                            elf64_image_t *image) {
    if (image->pages == NULL) return true;
    for (size_t i = 0; i < image->page_count; i++) {
        elf64_page_t *page = &image->pages[i];
        if (page->phys == 0) continue;
        if (!vmm_address_space_unmap_user(space, page->virt)) return false;
        pmm_free_frame(page->phys);
        page->phys = 0;
    }
    kfree(image->pages);
    image->pages = NULL;
    image->page_count = 0;
    image->entry = 0;
    return true;
}

bool elf64_load(vmm_address_space_t *space, const void *data, size_t size,
                elf64_image_t *image) {
    IRQ_GUARD;
    if (space == NULL || space->root_phys == 0 || data == NULL ||
        image == NULL || size < sizeof(elf64_header_t)) return false;
    image->entry = 0;
    image->page_count = 0;
    image->pages = NULL;

    const uint8_t *file = data;
    elf64_header_t header;
    memcpy(&header, file, sizeof(header));
    if (memcmp(header.ident, "\x7f" "ELF", 4) != 0 ||
        header.ident[4] != ELFCLASS64 || header.ident[5] != ELFDATA2LSB ||
        header.ident[6] != 1 || header.type != ET_EXEC ||
        header.machine != EM_X86_64 || header.version != 1 ||
        header.ehsize != sizeof(elf64_header_t) ||
        header.phentsize != sizeof(elf64_phdr_t) || header.phnum == 0 ||
        header.phnum > 128 ||
        !range_in_file(header.phoff,
            (uint64_t)header.phnum * header.phentsize, size)) return false;

    uint64_t low_page = ELF_USER_TOP;
    uint64_t high_page = 0;
    bool entry_is_executable = false;
    uint32_t load_segments = 0;
    for (uint16_t i = 0; i < header.phnum; i++) {
        elf64_phdr_t phdr;
        read_program_header(file, &header, i, &phdr);
        if (phdr.type == PT_DYNAMIC || phdr.type == PT_INTERP) return false;
        if (phdr.type != PT_LOAD) continue;
        if (phdr.memsz == 0 || phdr.filesz > phdr.memsz ||
            !range_in_file(phdr.offset, phdr.filesz, size) ||
            phdr.vaddr < PAGE_SIZE || phdr.vaddr >= ELF_USER_TOP ||
            phdr.memsz > ELF_USER_TOP - phdr.vaddr ||
            ((phdr.flags & (PF_W | PF_X)) == (PF_W | PF_X))) return false;
        if (phdr.align > 1 && (!power_of_two(phdr.align) ||
            (phdr.vaddr & (phdr.align - 1)) !=
            (phdr.offset & (phdr.align - 1)))) return false;

        uint64_t first = page_floor(phdr.vaddr);
        uint64_t last = page_floor(phdr.vaddr + phdr.memsz - 1);
        if (first < low_page) low_page = first;
        if (last + PAGE_SIZE > high_page) high_page = last + PAGE_SIZE;
        if ((phdr.flags & PF_X) && header.entry >= phdr.vaddr &&
            header.entry < phdr.vaddr + phdr.memsz) entry_is_executable = true;
        load_segments++;
    }
    if (load_segments == 0 || !entry_is_executable || high_page <= low_page)
        return false;

    size_t page_count = (size_t)((high_page - low_page) / PAGE_SIZE);
    if (page_count == 0 || page_count > ELF64_MAX_LOAD_PAGES) return false;
    image->pages = kzalloc(page_count * sizeof(*image->pages));
    if (image->pages == NULL) return false;
    image->page_count = page_count;
    image->entry = header.entry;
    for (size_t i = 0; i < page_count; i++) {
        image->pages[i].virt = low_page + (uint64_t)i * PAGE_SIZE;
        image->pages[i].permissions = VMM_NX;
    }

    /* First accumulate final segment rights per page; reject any overlap
     * which would make one page both writable and executable.
     */
    for (uint16_t i = 0; i < header.phnum; i++) {
        elf64_phdr_t phdr;
        read_program_header(file, &header, i, &phdr);
        if (phdr.type != PT_LOAD) continue;
        uint64_t first = page_floor(phdr.vaddr);
        uint64_t end = page_floor(phdr.vaddr + phdr.memsz - 1);
        uint64_t permissions = (phdr.flags & PF_W) ? VMM_WRITE : 0;
        for (uint64_t virt = first; virt <= end; virt += PAGE_SIZE) {
            size_t slot = (size_t)((virt - low_page) / PAGE_SIZE);
            image->pages[slot].permissions |= permissions;
            if (phdr.flags & PF_X) image->pages[slot].permissions &= ~VMM_NX;
            if ((image->pages[slot].permissions & VMM_WRITE) &&
                !(image->pages[slot].permissions & VMM_NX)) {
                release_partial(space, image);
                return false;
            }
        }
    }

    for (size_t i = 0; i < page_count; i++) {
        elf64_page_t *page = &image->pages[i];
        bool used = false;
        for (uint16_t p = 0; p < header.phnum && !used; p++) {
            elf64_phdr_t phdr;
            read_program_header(file, &header, p, &phdr);
            if (phdr.type != PT_LOAD || phdr.memsz == 0) continue;
            used = page->virt >= page_floor(phdr.vaddr) &&
                   page->virt <= page_floor(phdr.vaddr + phdr.memsz - 1);
        }
        if (!used) continue;
        uintptr_t phys = pmm_alloc_frame();
        if (phys == 0) {
            release_partial(space, image);
            return false;
        }
        if (!vmm_address_space_map_user(space, page->virt, phys,
                VMM_PRESENT | VMM_WRITE | VMM_USER | VMM_NX)) {
            pmm_free_frame(phys);
            release_partial(space, image);
            return false;
        }
        page->phys = phys;
    }

    if (!vmm_address_space_switch(space)) {
        release_partial(space, image);
        return false;
    }
    for (size_t i = 0; i < page_count; i++) {
        if (image->pages[i].phys != 0)
            memset((void *)(uintptr_t)image->pages[i].virt, 0, PAGE_SIZE);
    }
    for (uint16_t i = 0; i < header.phnum; i++) {
        elf64_phdr_t phdr;
        read_program_header(file, &header, i, &phdr);
        if (phdr.type != PT_LOAD || phdr.filesz == 0) continue;
        memcpy((void *)(uintptr_t)phdr.vaddr, file + phdr.offset,
               (size_t)phdr.filesz);
    }
    bool protected_all = true;
    for (size_t i = 0; i < page_count; i++) {
        elf64_page_t *page = &image->pages[i];
        if (page->phys == 0) continue;
        uint64_t flags = VMM_PRESENT | VMM_USER | page->permissions;
        if (!vmm_protect(page->virt, flags)) protected_all = false;
    }
    if (!vmm_address_space_switch(vmm_kernel_address_space())) {
        release_partial(space, image);
        return false;
    }
    if (!protected_all) {
        release_partial(space, image);
        return false;
    }
    return true;
}

bool elf64_unload(vmm_address_space_t *space, elf64_image_t *image) {
    IRQ_GUARD;
    if (space == NULL || image == NULL || image->pages == NULL ||
        (read_cr3() & 0x000FFFFFFFFFF000ULL) ==
        (space->root_phys & 0x000FFFFFFFFFF000ULL)) return false;
    return release_partial(space, image);
}
