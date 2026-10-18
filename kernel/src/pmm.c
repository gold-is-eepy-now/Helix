/* pmm.c — bitmap physical frame allocator. */
#include "pmm.h"

#include <stdbool.h>

#include <limine.h>

#include "kstring.h"

#define LOW_RESERVED_FRAMES (1024u * 1024u / PAGE_SIZE)

static uint8_t *g_bitmap;
static uint64_t g_bitmap_bytes;
static uint64_t g_bitmap_phys;
static uint64_t g_bitmap_frames;
static uint64_t g_top_frame;
static uint64_t g_usable_frames;  /* allocatable frames, including bitmap storage */
static uint64_t g_allocated;      /* currently allocated, including bitmap storage */
static uint64_t g_search_hint;

static inline void bit_set(uint64_t frame) {
    g_bitmap[frame >> 3] |= (uint8_t)(1u << (frame & 7u));
}

static inline void bit_clear(uint64_t frame) {
    g_bitmap[frame >> 3] &= (uint8_t)~(1u << (frame & 7u));
}

static inline bool bit_test(uint64_t frame) {
    return (g_bitmap[frame >> 3] >> (frame & 7u)) & 1u;
}

void pmm_init(struct limine_memmap_response *memmap, uint64_t hhdm_offset) {
    g_bitmap = NULL;
    g_bitmap_bytes = 0;
    g_bitmap_phys = 0;
    g_bitmap_frames = 0;
    g_top_frame = 0;
    g_usable_frames = 0;
    g_allocated = 0;
    g_search_hint = 0;

    if (memmap == NULL || hhdm_offset == 0) {
        return;
    }

    /* Find the complete physical span before sizing the bitmap. */
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        const struct limine_memmap_entry *entry = memmap->entries[i];
        if (entry->type != LIMINE_MEMMAP_USABLE || entry->length > UINT64_MAX - entry->base) {
            continue;
        }
        uint64_t end = entry->base + entry->length;
        uint64_t last = end / PAGE_SIZE;
        if (last > g_top_frame) {
            g_top_frame = last;
        }
    }
    if (g_top_frame == 0 || g_top_frame > UINT64_MAX - 7) {
        g_top_frame = 0;
        return;
    }

    g_bitmap_bytes = (g_top_frame + 7) / 8;
    if (g_bitmap_bytes > UINT64_MAX - (PAGE_SIZE - 1)) {
        g_top_frame = 0;
        g_bitmap_bytes = 0;
        return;
    }
    g_bitmap_frames = (g_bitmap_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    /* Place the bitmap inside one usable run; it is then reserved like any
     * other kernel allocation and is reachable through Limine's HHDM. */
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        const struct limine_memmap_entry *entry = memmap->entries[i];
        if (entry->type != LIMINE_MEMMAP_USABLE || entry->length > UINT64_MAX - entry->base) {
            continue;
        }
        uint64_t first = (entry->base + PAGE_SIZE - 1) / PAGE_SIZE;
        uint64_t last = (entry->base + entry->length) / PAGE_SIZE;
        if (first < LOW_RESERVED_FRAMES) {
            first = LOW_RESERVED_FRAMES;
        }
        if (last >= first && last - first >= g_bitmap_frames) {
            g_bitmap_phys = first * PAGE_SIZE;
            g_bitmap = (uint8_t *)(uintptr_t)(g_bitmap_phys + hhdm_offset);
            break;
        }
    }
    if (g_bitmap == NULL) {
        g_top_frame = 0;
        g_bitmap_bytes = 0;
        g_bitmap_frames = 0;
        return;
    }

    memset(g_bitmap, 0xFF, g_bitmap_bytes); /* everything starts reserved */
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        const struct limine_memmap_entry *entry = memmap->entries[i];
        if (entry->type != LIMINE_MEMMAP_USABLE || entry->length > UINT64_MAX - entry->base) {
            continue;
        }
        uint64_t first = (entry->base + PAGE_SIZE - 1) / PAGE_SIZE;
        uint64_t last = (entry->base + entry->length) / PAGE_SIZE;
        if (first < LOW_RESERVED_FRAMES) {
            first = LOW_RESERVED_FRAMES;
        }
        for (uint64_t frame = first; frame < last; frame++) {
            if (!bit_test(frame)) {
                continue; /* tolerate overlapping usable entries */
            }
            bit_clear(frame);
            g_usable_frames++;
        }
    }

    uint64_t bitmap_first = g_bitmap_phys / PAGE_SIZE;
    for (uint64_t i = 0; i < g_bitmap_frames; i++) {
        uint64_t frame = bitmap_first + i;
        if (!bit_test(frame)) {
            bit_set(frame);
            g_allocated++;
        }
    }
    g_search_hint = LOW_RESERVED_FRAMES;
}

uintptr_t pmm_alloc_frame(void) {
    if (g_bitmap == NULL) return 0;
    for (uint64_t i = g_search_hint; i < g_top_frame; i++) {
        if (!bit_test(i)) {
            bit_set(i); g_allocated++; g_search_hint = i + 1;
            return (uintptr_t)(i * PAGE_SIZE);
        }
    }
    for (uint64_t i = 0; i < g_search_hint && i < g_top_frame; i++) {
        if (!bit_test(i)) {
            bit_set(i); g_allocated++; g_search_hint = i + 1;
            return (uintptr_t)(i * PAGE_SIZE);
        }
    }
    return 0;
}

uintptr_t pmm_alloc_frames(size_t count) {
    if (g_bitmap == NULL || count == 0 || count > g_top_frame) return 0;
    uint64_t run = 0;
    for (uint64_t i = 0; i < g_top_frame; i++) {
        if (bit_test(i)) { run = 0; continue; }
        if (++run == count) {
            uint64_t start = i + 1 - count;
            for (uint64_t j = start; j <= i; j++) bit_set(j);
            g_allocated += count; g_search_hint = i + 1;
            return (uintptr_t)(start * PAGE_SIZE);
        }
    }
    return 0;
}

void pmm_free_frame(uintptr_t phys) { pmm_free_frames(phys, 1); }

void pmm_free_frames(uintptr_t phys, size_t count) {
    if (g_bitmap == NULL || phys % PAGE_SIZE != 0) return;
    uint64_t first = phys / PAGE_SIZE;
    for (size_t i = 0; i < count && first + i >= first; i++) {
        uint64_t frame = first + i;
        if (frame < g_top_frame && bit_test(frame) &&
            !(frame >= g_bitmap_phys / PAGE_SIZE && frame - g_bitmap_phys / PAGE_SIZE < g_bitmap_frames)) {
            bit_clear(frame);
            if (g_allocated > 0) g_allocated--;
        }
    }
    if (first < g_search_hint) g_search_hint = first;
}

uint64_t pmm_total_frames(void)     { return g_top_frame; }
uint64_t pmm_usable_frames(void)    { return g_usable_frames; }
uint64_t pmm_allocated_frames(void) { return g_allocated; }
uint64_t pmm_free_frame_count(void) { return g_usable_frames - g_allocated; }
uint64_t pmm_total_bytes(void)      { return g_top_frame * PAGE_SIZE; }
uint64_t pmm_free_bytes(void)       { return (g_usable_frames - g_allocated) * PAGE_SIZE; }
uint64_t pmm_bitmap_bytes(void)     { return g_bitmap_bytes; }
