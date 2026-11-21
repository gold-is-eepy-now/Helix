/* Physical frame allocator. One bit per 4 KiB frame, set = in use. Built from Limine's memory map:
 * everything starts in use, and only the frames inside "usable" entries are freed. Frame 0 is never
 * handed out, so a null physical address always means "none". Covers physical memory below 4 GiB;
 * usable memory above that is ignored for now (documented limit). */
#include <stdint.h>
#include <stddef.h>
#include "limine.h"
#include "loom_kernel.h"
#include <assert.h>

#define PMM_FRAME       4096ull
#define PMM_LIMIT       (4ull << 30)
#define PMM_FRAMES      (PMM_LIMIT / PMM_FRAME)
#define PMM_WORDS       (PMM_FRAMES / 64)

static uint64_t g_bitmap[PMM_WORDS];   /* 128 KiB in .bss */
static uint64_t g_free;
static uint64_t g_hint;

static inline void set_used(uint64_t f)  { g_bitmap[f / 64] |=  (1ull << (f % 64)); }
static inline void set_free(uint64_t f)  { g_bitmap[f / 64] &= ~(1ull << (f % 64)); }
static inline int  is_used(uint64_t f)   { return (g_bitmap[f / 64] >> (f % 64)) & 1; }

void pmm_init(const struct limine_memmap_response *mm) {
    for (uint64_t i = 0; i < PMM_WORDS; i++) g_bitmap[i] = ~0ull;   /* all used */
    g_free = 0;
    for (uint64_t e = 0; e < mm->entry_count; e++) {
        const struct limine_memmap_entry *ent = mm->entries[e];
        if (ent->type != LIMINE_MEMMAP_USABLE) continue;
        uint64_t first = (ent->base + PMM_FRAME - 1) / PMM_FRAME;       /* round up */
        uint64_t end   = (ent->base + ent->length) / PMM_FRAME;         /* round down */
        for (uint64_t f = first; f < end && f < PMM_FRAMES; f++) {
            if (f == 0) continue;
            set_free(f);
            g_free++;
        }
    }
    g_hint = 1;
}

uint64_t pmm_alloc_frame(void) {
    for (uint64_t n = 0; n < PMM_FRAMES; n++) {
        uint64_t f = (g_hint + n) % PMM_FRAMES;
        if (f == 0 || is_used(f)) continue;
        set_used(f);
        g_free--;
        g_hint = f + 1;
        return f * PMM_FRAME;
    }
    return 0;
}

void pmm_free_frame(uint64_t phys) {
    uint64_t f = phys / PMM_FRAME;
    assert(phys % PMM_FRAME == 0);
    assert(f < PMM_FRAMES && is_used(f));       /* double free or wild free is a bug */
    set_free(f);
    g_free++;
}

uint64_t pmm_free_count(void) { return g_free; }
