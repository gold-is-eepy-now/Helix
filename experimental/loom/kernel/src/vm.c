/* Page tables: one address space per Cell. Every address space shares the kernel's half (PML4
 * entries 256..511: the higher-half direct map and the kernel image), and has its own lower half.
 * Only the kernel's own tables are reached through the direct map, so the rules here are: map user
 * pages in the lower half only, and never write into a table shared with another space. */
#include <stdint.h>
#include <string.h>
#include "loom_kernel.h"
#include <assert.h>

#define PTE_P    (1ull << 0)
#define PTE_W    (1ull << 1)
#define PTE_U    (1ull << 2)
#define PTE_NX   (1ull << 63)
#define ADDR_MASK 0x000ffffffffff000ull

static uint64_t *table(uint64_t phys) { return (uint64_t *)phys_to_virt(phys); }

static inline unsigned idx(uint64_t virt, int level) {   /* level 4 = PML4 ... 1 = PT */
    return (unsigned)((virt >> (12 + 9 * (level - 1))) & 0x1FF);
}

uint64_t vm_current_cr3(void) {
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    return cr3 & ADDR_MASK;
}

uint64_t vm_new_space(void) {
    uint64_t pml4 = pmm_alloc_frame();
    if (pml4 == 0) return 0;
    uint64_t *dst = table(pml4);
    memset(dst, 0, 4096);
    uint64_t *src = table(vm_current_cr3());
    for (int i = 256; i < 512; i++) dst[i] = src[i];   /* shared kernel half */
    return pml4;
}

/* Returns the PTE for `virt`, creating intermediate tables if `create` is set. */
static uint64_t *pte_for(uint64_t pml4, uint64_t virt, int create, uint64_t inter_flags) {
    uint64_t *t = table(pml4);
    for (int level = 4; level > 1; level--) {
        uint64_t *e = &t[idx(virt, level)];
        if (!(*e & PTE_P)) {
            if (!create) return NULL;
            uint64_t next = pmm_alloc_frame();
            if (next == 0) return NULL;
            memset(table(next), 0, 4096);
            *e = next | inter_flags | PTE_P;
        }
        t = table(*e & ADDR_MASK);
    }
    return &t[idx(virt, 1)];
}

bool vm_map_user(uint64_t pml4, uint64_t virt, uint64_t phys, uint64_t flags) {
    assert(virt < 0x0000800000000000ull);                  /* lower half only */
    assert((virt & 0xFFF) == 0 && (phys & 0xFFF) == 0);
    uint64_t *pte = pte_for(pml4, virt, 1, PTE_W | PTE_U);
    if (pte == NULL || (*pte & PTE_P)) return false;       /* out of tables, or already mapped */
    *pte = phys | flags | PTE_P | PTE_U;
    return true;
}

uint64_t vm_translate(uint64_t pml4, uint64_t virt) {
    uint64_t *pte = pte_for(pml4, virt, 0, 0);
    if (pte == NULL || !(*pte & PTE_P)) return 0;
    return (*pte & ADDR_MASK) | (virt & 0xFFF);
}

uint64_t vm_unmap_user(uint64_t pml4, uint64_t virt) {
    uint64_t *pte = pte_for(pml4, virt, 0, 0);
    if (pte == NULL || !(*pte & PTE_P)) return 0;
    uint64_t phys = *pte & ADDR_MASK;
    *pte = 0;
    __asm__ volatile ("invlpg (%0)" : : "r"(virt) : "memory");
    return phys;
}

void vm_switch(uint64_t pml4) {
    __asm__ volatile ("mov %0, %%cr3" : : "r"(pml4) : "memory");
}

/* Frees every frame the lower half refers to, its tables, and the PML4. The caller must not be
 * running in this space. Everything in the lower half was allocated by vm_map_user or
 * vm_new_space, so freeing it cannot touch the kernel. */
static void free_level(uint64_t phys, int level) {
    uint64_t *t = table(phys);
    for (int i = 0; i < 512; i++) {
        if (level == 4 && i >= 256) continue;               /* shared kernel half */
        if (!(t[i] & PTE_P)) continue;
        uint64_t child = t[i] & ADDR_MASK;
        if (level > 1) free_level(child, level - 1);
        else pmm_free_frame(child);                          /* leaf: a user page */
    }
    pmm_free_frame(phys);
}

void vm_free_space(uint64_t pml4) {
    assert(pml4 != vm_current_cr3());
    free_level(pml4, 4);
}
