/* vmm.c — x86_64 four-level page tables.
 *
 * x86_64 translates a virtual address through four levels of tables:
 *
 *   PML4  -> PDPT  ->  PD  ->  PT  -> physical page
 *   9 bits   9 bits   9 bits  9 bits   (plus a 12-bit offset)
 *
 * Limine already built a table for the kernel and the higher-half direct map
 * (HHDM). We adopt that table (its physical address is in CR3) and modify it
 * in place: mapping a new page walks the levels, allocating a zeroed frame for
 * any missing table, and finally writes the leaf entry. Unmapping clears the
 * leaf entry; protecting rewrites its flags. Every change invalidates the
 * matching TLB entry with `invlpg`, or the CPU may keep using the old
 * translation.
 *
 * Because page-table frames are physical addresses, we reach them through the
 * HHDM: physical P is at P + hhdm_offset.
 */
#include "vmm.h"

#include <stddef.h>

#include "io.h"
#include "kstring.h"
#include "pmm.h"
#include "sync.h"

#define ADDR_MASK 0x000FFFFFFFFFF000ULL
#define FLAG_MASK (~ADDR_MASK)

static uint64_t g_hhdm;
static uint64_t *g_pml4;
static vmm_address_space_t g_kernel_space;

static uint64_t *table_at(uint64_t phys);

void vmm_init(uint64_t hhdm_offset) {
    IRQ_GUARD;
    g_hhdm = hhdm_offset;
    g_kernel_space.root_phys = read_cr3() & ADDR_MASK;
    g_pml4 = table_at(g_kernel_space.root_phys);
}

static uint64_t *table_at(uint64_t phys) {
    return (uint64_t *)(phys + g_hhdm);
}

/* Follow `entry` to the next level, creating (and zeroing) the table if
 * `create` is set. A huge page where we need a table is not handled here. */
static void allow_table_access(uint64_t *entry, uint64_t flags) {
    if (flags & VMM_USER) *entry |= VMM_USER;
    if (flags & VMM_WRITE) *entry |= VMM_WRITE;
}

static uint64_t *next_table(uint64_t *entry, bool create, uint64_t flags) {
    if (*entry & VMM_PRESENT) {
        if (*entry & VMM_HUGE) {
            return NULL;
        }
        allow_table_access(entry, flags);
        return table_at(*entry & ADDR_MASK);
    }
    if (!create) {
        return NULL;
    }
    uintptr_t frame = pmm_alloc_frame();
    if (frame == 0) {
        return NULL;
    }
    memset(table_at(frame), 0, PAGE_SIZE);
    *entry = frame | VMM_PRESENT | VMM_WRITE | (flags & VMM_USER);
    return table_at(frame);
}

/* Follow a page-directory entry to its page table, splitting a 2 MiB huge
 * page into 512 normal pages first if one is in the way. */
static uint64_t *page_table(uint64_t *pd_entry, bool create, uint64_t flags) {
    if (*pd_entry & VMM_PRESENT) {
        if (*pd_entry & VMM_HUGE) {
            if (!create) {
                return NULL;
            }
            uintptr_t frame = pmm_alloc_frame();
            if (frame == 0) {
                return NULL;
            }
            uint64_t *pt = table_at(frame);
            uint64_t base = *pd_entry & 0x000FFFFFFFE00000ULL;
            uint64_t huge_flags = (*pd_entry & 0xFFFULL) & ~VMM_HUGE;
            for (uint64_t i = 0; i < 512; i++) {
                pt[i] = (base + i * PAGE_SIZE) | huge_flags | VMM_PRESENT;
            }
            *pd_entry = frame |
                (huge_flags & (VMM_PRESENT | VMM_WRITE | VMM_USER)) |
                VMM_PRESENT;
            allow_table_access(pd_entry, flags);
            return pt;
        }
        allow_table_access(pd_entry, flags);
        return table_at(*pd_entry & ADDR_MASK);
    }
    if (!create) {
        return NULL;
    }
    uintptr_t frame = pmm_alloc_frame();
    if (frame == 0) {
        return NULL;
    }
    memset(table_at(frame), 0, PAGE_SIZE);
    *pd_entry = frame | VMM_PRESENT | VMM_WRITE | (flags & VMM_USER);
    return table_at(frame);
}

/* Walk the tables and return a pointer to the leaf entry for `virt`. */
static uint64_t *walk(uint64_t *root, uint64_t virt, bool create,
                      uint64_t flags) {
    uint64_t i4 = (virt >> 39) & 0x1FF;
    uint64_t i3 = (virt >> 30) & 0x1FF;
    uint64_t i2 = (virt >> 21) & 0x1FF;
    uint64_t i1 = (virt >> 12) & 0x1FF;

    uint64_t *pdpt = next_table(&root[i4], create, flags);
    if (pdpt == NULL) {
        return NULL;
    }
    uint64_t *pd = next_table(&pdpt[i3], create, flags);
    if (pd == NULL) {
        return NULL;
    }
    uint64_t *pt = page_table(&pd[i2], create, flags);
    if (pt == NULL) {
        return NULL;
    }
    return &pt[i1];
}

bool vmm_map(uint64_t virt, uint64_t phys, uint64_t flags) {
    IRQ_GUARD;
    uint64_t *pte = walk(g_pml4, virt, true, flags);
    if (pte == NULL) {
        return false;
    }
    *pte = (phys & ADDR_MASK) | (flags & FLAG_MASK) | VMM_PRESENT;
    invlpg(virt);
    return true;
}

bool vmm_unmap(uint64_t virt) {
    IRQ_GUARD;
    uint64_t *pte = walk(g_pml4, virt, false, 0);
    if (pte == NULL || !(*pte & VMM_PRESENT)) {
        return false;
    }
    *pte = 0;
    invlpg(virt);
    return true;
}

bool vmm_protect(uint64_t virt, uint64_t flags) {
    IRQ_GUARD;
    uint64_t *pte = walk(g_pml4, virt, false, flags);
    if (pte == NULL || !(*pte & VMM_PRESENT)) {
        return false;
    }
    *pte = (*pte & ADDR_MASK) | (flags & FLAG_MASK) | VMM_PRESENT;
    invlpg(virt);
    return true;
}

bool vmm_get_mapping(uint64_t virt, uint64_t *phys_out, uint64_t *flags_out) {
    IRQ_GUARD;
    uint64_t *pte = walk(g_pml4, virt, false, 0);
    if (pte == NULL || !(*pte & VMM_PRESENT)) {
        return false;
    }
    if (phys_out != NULL) {
        *phys_out = *pte & ADDR_MASK;
    }
    if (flags_out != NULL) {
        *flags_out = *pte & FLAG_MASK;
    }
    return true;
}

static void free_page_table_tree(uint64_t table_phys, unsigned int level) {
    uint64_t *table = table_at(table_phys);
    for (size_t i = 0; i < 512; i++) {
        uint64_t entry = table[i];
        if (!(entry & VMM_PRESENT) || level == 1 || (entry & VMM_HUGE))
            continue;
        free_page_table_tree(entry & ADDR_MASK, level - 1);
    }
    pmm_free_frame((uintptr_t)table_phys);
}

bool vmm_address_space_create(vmm_address_space_t *space) {
    IRQ_GUARD;
    if (space == NULL || space == &g_kernel_space || g_pml4 == NULL)
        return false;
    space->root_phys = 0;
    uintptr_t root_phys = pmm_alloc_frame();
    if (root_phys == 0) return false;
    uint64_t *root = table_at(root_phys);
    memset(root, 0, PAGE_SIZE);
    /* x86_64's upper canonical half starts at PML4 slot 256. Share those
     * supervisor mappings so kernel code, data, HHDM and stacks remain live.
     */
    for (size_t i = 256; i < 512; i++) root[i] = g_pml4[i];
    space->root_phys = root_phys;
    return true;
}

bool vmm_address_space_destroy(vmm_address_space_t *space) {
    IRQ_GUARD;
    if (space == NULL || space == &g_kernel_space || space->root_phys == 0 ||
        (read_cr3() & ADDR_MASK) == space->root_phys) return false;
    uint64_t *root = table_at(space->root_phys);
    /* Only lower-half trees belong to this address space. Upper-half entries
     * are shared with the kernel root and must never be freed here.
     */
    for (size_t i = 0; i < 256; i++) {
        if ((root[i] & VMM_PRESENT) && !(root[i] & VMM_HUGE))
            free_page_table_tree(root[i] & ADDR_MASK, 3);
    }
    pmm_free_frame((uintptr_t)space->root_phys);
    space->root_phys = 0;
    return true;
}

bool vmm_address_space_switch(vmm_address_space_t *space) {
    IRQ_GUARD;
    if (space == NULL || space->root_phys == 0) return false;
    write_cr3(space->root_phys);
    g_pml4 = table_at(space->root_phys);
    return true;
}

vmm_address_space_t *vmm_kernel_address_space(void) {
    return &g_kernel_space;
}

static bool valid_user_mapping(uint64_t virt, uint64_t phys, uint64_t flags) {
    const uint64_t allowed = VMM_PRESENT | VMM_WRITE | VMM_USER | VMM_NX;
    return virt < 0x0000800000000000ULL &&
           (virt & (PAGE_SIZE - 1)) == 0 &&
           (phys & (PAGE_SIZE - 1)) == 0 && (flags & VMM_USER) != 0 &&
           (flags & ~allowed) == 0;
}

bool vmm_address_space_map_user(vmm_address_space_t *space, uint64_t virt,
                                uint64_t phys, uint64_t flags) {
    IRQ_GUARD;
    if (space == NULL || space->root_phys == 0 ||
        !valid_user_mapping(virt, phys, flags)) return false;
    uint64_t *root = table_at(space->root_phys);
    uint64_t *pte = walk(root, virt, true, flags);
    if (pte == NULL || (*pte & VMM_PRESENT)) return false;
    *pte = (phys & ADDR_MASK) | (flags & FLAG_MASK) | VMM_PRESENT;
    if ((read_cr3() & ADDR_MASK) == space->root_phys) invlpg(virt);
    return true;
}

bool vmm_address_space_unmap_user(vmm_address_space_t *space, uint64_t virt) {
    IRQ_GUARD;
    if (space == NULL || space->root_phys == 0 ||
        virt >= 0x0000800000000000ULL ||
        (virt & (PAGE_SIZE - 1)) != 0) return false;
    uint64_t *pte = walk(table_at(space->root_phys), virt, false, 0);
    if (pte == NULL || !(*pte & VMM_PRESENT) || !(*pte & VMM_USER))
        return false;
    *pte = 0;
    if ((read_cr3() & ADDR_MASK) == space->root_phys) invlpg(virt);
    return true;
}

bool vmm_address_space_get_mapping(vmm_address_space_t *space, uint64_t virt,
                                   uint64_t *phys_out, uint64_t *flags_out) {
    IRQ_GUARD;
    if (space == NULL || space->root_phys == 0) return false;
    uint64_t *pte = walk(table_at(space->root_phys), virt, false, 0);
    if (pte == NULL || !(*pte & VMM_PRESENT)) return false;
    if (phys_out != NULL) *phys_out = *pte & ADDR_MASK;
    if (flags_out != NULL) *flags_out = *pte & FLAG_MASK;
    return true;
}
