/* pmm.c — bitmap physical frame allocator.
 *
 * One bit per 4 KiB frame: 1 = in use, 0 = free. We start with every bit set
 * (all memory in use) and clear the bits that the bootloader marks USABLE,
 * then re-reserve the low 1 MiB, which is full of firmware structures and is
 * never worth handing out.
 *
 * The bitmap is a static array, which caps us at 4 GiB of physical RAM for
 * now. That is plenty for a long while, and a dynamically sized bitmap is a
 * straightforward later change.
 */
#include "pmm.h"

#include <stdbool.h>

#include <limine.h>

#include "kstring.h"

#define MAX_PHYS_BYTES (4ull * 1024 * 1024 * 1024)
#define MAX_FRAMES     (MAX_PHYS_BYTES / PAGE_SIZE)
#define BITMAP_BYTES   (MAX_FRAMES / 8)
#define LOW_RESERVED_FRAMES (1024u * 1024u / PAGE_SIZE) /* first 1 MiB */

static uint8_t g_bitmap[BITMAP_BYTES];
static uint64_t g_top_frame;      /* one past the highest usable frame      */
static uint64_t g_usable_frames;  /* free at init (after reservations)      */
static uint64_t g_allocated;      /* frames handed out since init           */
static uint64_t g_search_hint;    /* where the last allocation left off     */

static inline void bit_set(uint64_t frame) {
    g_bitmap[frame >> 3] |= (uint8_t)(1u << (frame & 7u));
}

static inline void bit_clear(uint64_t frame) {
    g_bitmap[frame >> 3] &= (uint8_t)~(1u << (frame & 7u));
}

static inline bool bit_test(uint64_t frame) {
    return (g_bitmap[frame >> 3] >> (frame & 7u)) & 1u;
}

void pmm_init(struct limine_memmap_response *memmap) {
    memset(g_bitmap, 0xFF, sizeof(g_bitmap)); /* everything starts in use */
    g_top_frame = 0;
    g_usable_frames = 0;
    g_allocated = 0;
    g_search_hint = 0;

    if (memmap == NULL) {
        return;
    }

    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        const struct limine_memmap_entry *entry = memmap->entries[i];
        if (entry->type != LIMINE_MEMMAP_USABLE) {
            continue;
        }

        uint64_t base = entry->base;
        uint64_t end = entry->base + entry->length;
        if (end > MAX_PHYS_BYTES) {
            end = MAX_PHYS_BYTES;
        }

        uint64_t first = (base + PAGE_SIZE - 1) / PAGE_SIZE; /* round up   */
        uint64_t last = end / PAGE_SIZE;                     /* round down */

        for (uint64_t frame = first; frame < last; frame++) {
            if (frame < LOW_RESERVED_FRAMES) {
                continue; /* keep the low 1 MiB reserved */
            }
            bit_clear(frame);
            g_usable_frames++;
        }
        if (last > g_top_frame) {
            g_top_frame = last;
        }
    }
}

uintptr_t pmm_alloc_frame(void) {
    /* Search forward from the hint, then wrap around once. */
    for (uint64_t i = g_search_hint; i < g_top_frame; i++) {
        if (!bit_test(i)) {
            bit_set(i);
            g_allocated++;
            g_search_hint = i + 1;
            return (uintptr_t)(i * PAGE_SIZE);
        }
    }
    for (uint64_t i = 0; i < g_search_hint && i < g_top_frame; i++) {
        if (!bit_test(i)) {
            bit_set(i);
            g_allocated++;
            g_search_hint = i + 1;
            return (uintptr_t)(i * PAGE_SIZE);
        }
    }
    return 0;
}

uintptr_t pmm_alloc_frames(size_t count) {
    if (count == 0) {
        return 0;
    }
    uint64_t run = 0;
    for (uint64_t i = 0; i < g_top_frame; i++) {
        if (bit_test(i)) {
            run = 0;
            continue;
        }
        run++;
        if (run == count) {
            uint64_t start = i + 1 - count;
            for (uint64_t j = start; j <= i; j++) {
                bit_set(j);
            }
            g_allocated += count;
            g_search_hint = i + 1;
            return (uintptr_t)(start * PAGE_SIZE);
        }
    }
    return 0;
}

void pmm_free_frame(uintptr_t phys) {
    pmm_free_frames(phys, 1);
}

void pmm_free_frames(uintptr_t phys, size_t count) {
    uint64_t first = phys / PAGE_SIZE;
    for (size_t i = 0; i < count; i++) {
        uint64_t frame = first + i;
        if (frame < g_top_frame && bit_test(frame)) {
            bit_clear(frame);
            if (g_allocated > 0) {
                g_allocated--;
            }
        }
    }
    if (first < g_search_hint) {
        g_search_hint = first;
    }
}

uint64_t pmm_total_frames(void)     { return g_top_frame; }
uint64_t pmm_usable_frames(void)    { return g_usable_frames; }
uint64_t pmm_allocated_frames(void) { return g_allocated; }
uint64_t pmm_free_frame_count(void) { return g_usable_frames - g_allocated; }
uint64_t pmm_total_bytes(void)      { return g_top_frame * PAGE_SIZE; }
uint64_t pmm_free_bytes(void)       { return (g_usable_frames - g_allocated) * PAGE_SIZE; }
uint64_t pmm_bitmap_bytes(void)     { return sizeof(g_bitmap); }
