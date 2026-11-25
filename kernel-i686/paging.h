/* paging.h — i686 paging (PAE). Three levels: PDPT (4 entries) -> PD (512) -> PT (512).
 * Stage I2b: an identity map of RAM with 2 MiB pages, and 4 KiB mappings on demand. */
#ifndef HELIX_I686_PAGING_H
#define HELIX_I686_PAGING_H

#include <stdbool.h>
#include <stdint.h>

/* Identity-map [0, identity_top) with 2 MiB pages and turn paging on. identity_top is
 * rounded up to 2 MiB. Must run once, before any mapping call. */
bool paging_init(uint64_t identity_top);

/* Map one 4 KiB page: virtual `virt` (32-bit) to physical `phys`. Splits a 2 MiB page when
 * needed. Fails (false) if the page is already mapped or the table pool is exhausted. */
bool paging_map4k(uint32_t virt, uint64_t phys, uint64_t flags);

/* Remove a 4 KiB mapping. Returns false if it was not mapped as a 4 KiB page. */
bool paging_unmap4k(uint32_t virt);

/* The raw page-table entry for `virt`, or 0 if none (for tests). */
uint64_t paging_pte(uint32_t virt);

#define PAGE_PRESENT  (1ull << 0)
#define PAGE_WRITABLE (1ull << 1)

#endif /* HELIX_I686_PAGING_H */
