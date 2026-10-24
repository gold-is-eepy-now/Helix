/* vmm.h — virtual memory: our own page-table map/unmap/protect. */
#ifndef HELIX_VMM_H
#define HELIX_VMM_H

#include <stdbool.h>
#include <stdint.h>

#define VMM_PRESENT (1ULL << 0)
#define VMM_WRITE   (1ULL << 1)
#define VMM_USER    (1ULL << 2)
#define VMM_HUGE    (1ULL << 7)
#define VMM_NX      (1ULL << 63)

typedef struct {
    uint64_t root_phys;
} vmm_address_space_t;

/* Adopt the current address space (from CR3) and remember the direct-map
 * offset so we can reach page-table frames by physical address. */
void vmm_init(uint64_t hhdm_offset);

/* Address spaces get private lower-half page tables and share the kernel's
 * supervisor-only upper-half mappings. User leaf frames remain caller-owned.
 */
bool vmm_address_space_create(vmm_address_space_t *space);
bool vmm_address_space_destroy(vmm_address_space_t *space);
bool vmm_address_space_switch(vmm_address_space_t *space);
vmm_address_space_t *vmm_kernel_address_space(void);
bool vmm_address_space_map_user(vmm_address_space_t *space, uint64_t virt,
                                uint64_t phys, uint64_t flags);
bool vmm_address_space_unmap_user(vmm_address_space_t *space, uint64_t virt);
bool vmm_address_space_get_mapping(vmm_address_space_t *space, uint64_t virt,
                                   uint64_t *phys_out, uint64_t *flags_out);

/* Map one 4 KiB page. Returns false if a page table could not be allocated. */
bool vmm_map(uint64_t virt, uint64_t phys, uint64_t flags);

/* Remove a mapping. Returns false if it was not mapped. */
bool vmm_unmap(uint64_t virt);

/* Change the flags of an existing mapping. Returns false if not mapped. */
bool vmm_protect(uint64_t virt, uint64_t flags);

/* Query a mapping: physical address and flags. Returns false if not mapped. */
bool vmm_get_mapping(uint64_t virt, uint64_t *phys_out, uint64_t *flags_out);

#endif /* HELIX_VMM_H */
