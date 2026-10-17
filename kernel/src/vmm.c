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

#define ADDR_MASK 0x000FFFFFFFFFF000ULL
#define FLAG_MASK (~ADDR_MASK)

static uint64_t g_hhdm;
static uint64_t *g_pml4;

void vmm_init(uint64_t hhdm_offset) {
    g_hhdm = hhdm_offset;
    g_pml4 = (uint64_t *)(read_cr3() + g_hhdm);
}

static uint64_t *table_at(uint64_t phys) {
    return (uint64_t *)(phys + g_hhdm);
}

/* Follow `entry` to the next level, creating (and zeroing) the table if
 * `create` is set. A huge page where we need a table is not handled here. */
static uint64_t *next_table(uint64_t *entry, bool create) {
    if (*entry & VMM_PRESENT) {
        if (*entry & VMM_HUGE) {
            return NULL;
        }
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
    *entry = frame | VMM_PRESENT | VMM_WRITE;
    return table_at(frame);
}

/* Follow a page-directory entry to its page table, splitting a 2 MiB huge
 * page into 512 normal pages first if one is in the way. */
static uint64_t *page_table(uint64_t *pd_entry, bool create) {
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
            uint64_t flags = (*pd_entry & 0xFFFULL) & ~VMM_HUGE;
            for (uint64_t i = 0; i < 512; i++) {
                pt[i] = (base + i * PAGE_SIZE) | flags | VMM_PRESENT;
            }
            *pd_entry = frame | (flags & (VMM_PRESENT | VMM_WRITE | VMM_USER)) | VMM_PRESENT;
            return pt;
        }
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
    *pd_entry = frame | VMM_PRESENT | VMM_WRITE;
    return table_at(frame);
}

/* Walk the tables and return a pointer to the leaf entry for `virt`. */
static uint64_t *walk(uint64_t virt, bool create) {
    uint64_t i4 = (virt >> 39) & 0x1FF;
    uint64_t i3 = (virt >> 30) & 0x1FF;
    uint64_t i2 = (virt >> 21) & 0x1FF;
    uint64_t i1 = (virt >> 12) & 0x1FF;

    uint64_t *pdpt = next_table(&g_pml4[i4], create);
    if (pdpt == NULL) {
        return NULL;
    }
    uint64_t *pd = next_table(&pdpt[i3], create);
    if (pd == NULL) {
        return NULL;
    }
    uint64_t *pt = page_table(&pd[i2], create);
    if (pt == NULL) {
        return NULL;
    }
    return &pt[i1];
}

bool vmm_map(uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t *pte = walk(virt, true);
    if (pte == NULL) {
        return false;
    }
    *pte = (phys & ADDR_MASK) | (flags & FLAG_MASK) | VMM_PRESENT;
    invlpg(virt);
    return true;
}

bool vmm_unmap(uint64_t virt) {
    uint64_t *pte = walk(virt, false);
    if (pte == NULL || !(*pte & VMM_PRESENT)) {
        return false;
    }
    *pte = 0;
    invlpg(virt);
    return true;
}

bool vmm_protect(uint64_t virt, uint64_t flags) {
    uint64_t *pte = walk(virt, false);
    if (pte == NULL || !(*pte & VMM_PRESENT)) {
        return false;
    }
    *pte = (*pte & ADDR_MASK) | (flags & FLAG_MASK) | VMM_PRESENT;
    invlpg(virt);
    return true;
}

bool vmm_get_mapping(uint64_t virt, uint64_t *phys_out, uint64_t *flags_out) {
    uint64_t *pte = walk(virt, false);
    if (pte == NULL || !(*pte & VMM_PRESENT)) {
        return false;
    }
    if (phys_out != NULL) {
        *phys_out = *pte & ADDR_MASK;
    }
    if (flags_out != NULL) {
        *flags_out = *pte & 0xFFFULL;
    }
    return true;
}
