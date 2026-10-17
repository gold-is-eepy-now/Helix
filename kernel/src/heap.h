/* heap.h — the kernel heap (kmalloc/kfree). */
#ifndef HELIX_HEAP_H
#define HELIX_HEAP_H

#include <stddef.h>
#include <stdint.h>

/* Initialise the heap. `hhdm_offset` is the Limine higher-half direct-map
 * offset: physical address P is reachable at P + hhdm_offset. */
void heap_init(uint64_t hhdm_offset);

/* Allocate `size` bytes (16-byte aligned), or NULL on failure. */
void *kmalloc(size_t size);

/* Allocate zeroed memory. */
void *kzalloc(size_t size);

/* Release memory previously returned by kmalloc/kzalloc. */
void kfree(void *ptr);

/* Statistics. */
uint64_t heap_used_bytes(void);
uint64_t heap_total_bytes(void);
uint64_t heap_block_count(void);

#endif /* HELIX_HEAP_H */
