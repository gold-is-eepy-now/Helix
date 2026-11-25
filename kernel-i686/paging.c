/* paging.c — i686 PAE paging.
 *
 * Why PAE: NX needs it on i686, and it allows physical addresses above 4 GiB later.
 * Tables are static arrays inside the kernel image, which is identity mapped, so a table's
 * physical address equals its pointer. CR3 must hold a 32-byte-aligned PDPT below 4 GiB. */
#include "paging.h"

#include <stddef.h>

#define PAGE_SIZE_4K  4096u
#define PAGE_SIZE_2M  (2u * 1024u * 1024u)
#define PAGE_SIZE_1G  (1024u * 1024u * 1024u)

#define PDE_LARGE     (1ull << 7)     /* PS: this PD entry maps a 2 MiB page */
#define ENTRY_ADDR    0x000FFFFFFFFFF000ull

#define PD_POOL       8
#define PT_POOL       16

static uint64_t g_pdpt[4] __attribute__((aligned(32)));
static uint64_t g_pd[PD_POOL][512] __attribute__((aligned(4096)));
static uint64_t g_pt[PT_POOL][512] __attribute__((aligned(4096)));
static unsigned g_pd_used;
static unsigned g_pt_used;

static inline void invlpg(uint32_t virt) {
    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

static uint64_t *alloc_pd(void) {
    if (g_pd_used == PD_POOL) return NULL;
    return g_pd[g_pd_used++];
}

static uint64_t *alloc_pt(void) {
    if (g_pt_used == PT_POOL) return NULL;
    return g_pt[g_pt_used++];
}

bool paging_init(uint64_t identity_top) {
    uint64_t top = (identity_top + PAGE_SIZE_2M - 1) & ~(uint64_t)(PAGE_SIZE_2M - 1);
    if (top == 0 || top > 0x100000000ull) {
        return false;
    }
    /* One PDPT entry (and one PD) per GiB that the identity map touches. */
    for (uint64_t gb = 0; gb * PAGE_SIZE_1G < top; gb++) {
        uint64_t *pd = alloc_pd();
        if (pd == NULL) return false;
        g_pdpt[gb] = (uint64_t)(uintptr_t)pd | PAGE_PRESENT;
        for (unsigned i = 0; i < 512; i++) {
            uint64_t phys = gb * PAGE_SIZE_1G + (uint64_t)i * PAGE_SIZE_2M;
            if (phys >= top) break;
            pd[i] = phys | PAGE_PRESENT | PAGE_WRITABLE | PDE_LARGE;
        }
    }

    uint32_t cr3 = (uint32_t)(uintptr_t)g_pdpt;
    uint32_t cr4, cr0;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1u << 5);                       /* CR4.PAE, must be set before CR0.PG */
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= (1u << 31);                      /* CR0.PG: paging on */
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
    return true;
}

/* Walk to the PT that holds `virt`, creating or splitting tables as needed. */
static uint64_t *pt_for(uint32_t virt, bool create) {
    unsigned pdpt_i = virt >> 30;
    if (pdpt_i >= 4) return NULL;
    if ((g_pdpt[pdpt_i] & PAGE_PRESENT) == 0) {
        if (!create) return NULL;
        uint64_t *pd = alloc_pd();
        if (pd == NULL) return NULL;
        g_pdpt[pdpt_i] = (uint64_t)(uintptr_t)pd | PAGE_PRESENT;
    }
    uint64_t *pd = (uint64_t *)(uintptr_t)(g_pdpt[pdpt_i] & ENTRY_ADDR);
    unsigned pd_i = (virt >> 21) & 511u;
    uint64_t pde = pd[pd_i];
    if ((pde & PAGE_PRESENT) == 0) {
        if (!create) return NULL;
        uint64_t *pt = alloc_pt();
        if (pt == NULL) return NULL;
        pd[pd_i] = (uint64_t)(uintptr_t)pt | PAGE_PRESENT | PAGE_WRITABLE;
        return pt;
    }
    if (pde & PDE_LARGE) {
        if (!create) return NULL;
        /* Split: a 4 KiB table that maps the same 2 MiB with the same permissions. */
        uint64_t *pt = alloc_pt();
        if (pt == NULL) return NULL;
        uint64_t base = pde & ENTRY_ADDR & ~(uint64_t)(PAGE_SIZE_2M - 1);
        uint64_t flags = (pde & (PAGE_PRESENT | PAGE_WRITABLE));
        for (unsigned i = 0; i < 512; i++) {
            pt[i] = (base + (uint64_t)i * PAGE_SIZE_4K) | flags;
        }
        pd[pd_i] = (uint64_t)(uintptr_t)pt | PAGE_PRESENT | PAGE_WRITABLE;
        return pt;
    }
    return (uint64_t *)(uintptr_t)(pde & ENTRY_ADDR);
}

bool paging_map4k(uint32_t virt, uint64_t phys, uint64_t flags) {
    uint64_t *pt = pt_for(virt, true);
    if (pt == NULL) return false;
    unsigned pt_i = (virt >> 12) & 511u;
    if (pt[pt_i] & PAGE_PRESENT) return false;
    pt[pt_i] = (phys & ENTRY_ADDR) | (flags & (PAGE_PRESENT | PAGE_WRITABLE)) | PAGE_PRESENT;
    invlpg(virt);
    return true;
}

bool paging_unmap4k(uint32_t virt) {
    uint64_t *pt = pt_for(virt, false);
    if (pt == NULL) return false;
    unsigned pt_i = (virt >> 12) & 511u;
    if ((pt[pt_i] & PAGE_PRESENT) == 0) return false;
    pt[pt_i] = 0;
    invlpg(virt);
    return true;
}

uint64_t paging_pte(uint32_t virt) {
    uint64_t *pt = pt_for(virt, false);
    if (pt == NULL) return 0;
    return pt[(virt >> 12) & 511u];
}
