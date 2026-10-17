/* heap.c — a free-list kernel heap backed by physical frames.
 *
 * The heap is a linked list of blocks embedded in the memory itself:
 *
 *     [ block_t header ][ payload ... ][ block_t header ][ payload ... ]
 *
 * `size` is the payload size; the header sits immediately before the pointer
 * we hand back. Allocation is first-fit with splitting; freeing coalesces
 * adjacent free blocks. When no block is big enough, the heap asks the PMM
 * for more contiguous frames and appends them as a new block.
 *
 * Physical frames are accessed through the higher-half direct map, so a
 * physical address from the PMM becomes a usable pointer via `phys + hhdm`.
 */
#include "heap.h"

#include <stdbool.h>

#include "kstring.h"
#include "pmm.h"

#define HEAP_ALIGN 16
#define GROW_CHUNK (256u * 1024u)

typedef struct block {
    size_t size;       /* payload bytes                              */
    bool free;
    struct block *next;
    struct block *prev;
} block_t;

static uint64_t g_hhdm;
static block_t *g_head;
static uint64_t g_used;
static uint64_t g_total;

static size_t align_up(size_t value, size_t align) {
    return (value + align - 1) & ~(align - 1);
}

static bool heap_grow(size_t min_payload);

void heap_init(uint64_t hhdm_offset) {
    g_hhdm = hhdm_offset;
    g_head = NULL;
    g_used = 0;
    g_total = 0;
    /* Reserve a first chunk so ordinary allocations do not each pay for a
     * frame run. */
    heap_grow(256u * 1024u);
}

static bool heap_grow(size_t min_payload) {
    size_t need = min_payload + sizeof(block_t);
    size_t frames = (need + PAGE_SIZE - 1) / PAGE_SIZE;

    uintptr_t phys = pmm_alloc_frames(frames);
    if (phys == 0) {
        return false;
    }

    block_t *block = (block_t *)(phys + g_hhdm);
    block->size = frames * PAGE_SIZE - sizeof(block_t);
    block->free = true;
    block->next = NULL;
    block->prev = NULL;

    if (g_head == NULL) {
        g_head = block;
    } else {
        block_t *tail = g_head;
        while (tail->next != NULL) {
            tail = tail->next;
        }
        tail->next = block;
        block->prev = tail;
    }

    g_total += frames * PAGE_SIZE;
    return true;
}

void *kmalloc(size_t size) {
    if (size == 0) {
        return NULL;
    }
    size_t need = align_up(size, HEAP_ALIGN);

    for (int attempt = 0; attempt < 2; attempt++) {
        for (block_t *block = g_head; block != NULL; block = block->next) {
            if (!block->free || block->size < need) {
                continue;
            }
            /* Split off the tail if the remainder can hold a header plus at
             * least one aligned payload. */
            if (block->size >= need + sizeof(block_t) + HEAP_ALIGN) {
                block_t *rest = (block_t *)((uint8_t *)block + sizeof(block_t) + need);
                rest->size = block->size - need - sizeof(block_t);
                rest->free = true;
                rest->next = block->next;
                rest->prev = block;
                if (block->next != NULL) {
                    block->next->prev = rest;
                }
                block->next = rest;
                block->size = need;
            }
            block->free = false;
            g_used += block->size;
            return (uint8_t *)block + sizeof(block_t);
        }

        /* Nothing big enough: grow and try once more. */
        size_t grow_by = need > GROW_CHUNK ? need : GROW_CHUNK;
        if (!heap_grow(grow_by)) {
            return NULL;
        }
    }
    return NULL;
}

void *kzalloc(size_t size) {
    void *ptr = kmalloc(size);
    if (ptr != NULL) {
        memset(ptr, 0, size);
    }
    return ptr;
}

void kfree(void *ptr) {
    if (ptr == NULL) {
        return;
    }
    block_t *block = (block_t *)((uint8_t *)ptr - sizeof(block_t));
    if (block->free) {
        return; /* double free: ignore rather than corrupt the heap */
    }
    block->free = true;
    g_used -= block->size;

    /* Coalesce with the next block. */
    if (block->next != NULL && block->next->free) {
        block_t *next = block->next;
        block->size += sizeof(block_t) + next->size;
        block->next = next->next;
        if (next->next != NULL) {
            next->next->prev = block;
        }
    }
    /* Coalesce with the previous block. */
    if (block->prev != NULL && block->prev->free) {
        block_t *prev = block->prev;
        prev->size += sizeof(block_t) + block->size;
        prev->next = block->next;
        if (block->next != NULL) {
            block->next->prev = prev;
        }
    }
}

uint64_t heap_used_bytes(void) {
    return g_used;
}

uint64_t heap_total_bytes(void) {
    return g_total;
}

uint64_t heap_block_count(void) {
    uint64_t count = 0;
    for (block_t *block = g_head; block != NULL; block = block->next) {
        count++;
    }
    return count;
}
