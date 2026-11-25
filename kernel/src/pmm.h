/* pmm.h — physical memory manager: a bitmap of 4 KiB frames.
 *
 * The bootloader hands us a map of physical memory tagged by type. The PMM
 * turns the USABLE parts of that map into a bitmap of frames and hands them
 * out one (or several, contiguous) at a time. Everything above this — the
 * heap, page tables, user-space memory — is built from frames.
 */
#ifndef HELIX_PMM_H
#define HELIX_PMM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PAGE_SIZE 4096u

/* One span of physical memory, as the boot code reports it. The boot protocol
 * (Limine on x86_64, Multiboot on i686) is translated into this list by the
 * boot code, so the allocator does not depend on either protocol. */
struct pmm_region {
    uint64_t base;      /* physical start */
    uint64_t length;    /* bytes */
    bool     usable;    /* true: may be handed out; false: reserved by firmware or the kernel */
};

/* Build the frame bitmap from a list of regions. The bitmap's storage is placed in one
 * usable run and reached through `hhdm_offset`: physical P is at P + hhdm_offset. An
 * identity-mapped kernel (i686 stage I2) passes 0. */
void pmm_init(const struct pmm_region *regions, size_t count, uint64_t hhdm_offset);

/* Allocate one frame; returns its physical address, or 0 if out of memory. */
uintptr_t pmm_alloc_frame(void);

/* Offset of the higher-half direct map: physical address P is at P + offset.
 * Device DMA uses physical addresses; the kernel reaches the same bytes this way. */
uint64_t pmm_hhdm_offset(void);

/* Allocate `count` physically contiguous frames; returns the base physical
 * address, or 0 if no run that long is free. */
uintptr_t pmm_alloc_frames(size_t count);

/* Return frames to the pool. */
void pmm_free_frame(uintptr_t phys);
void pmm_free_frames(uintptr_t phys, size_t count);

/* Statistics. */
uint64_t pmm_total_frames(void);
uint64_t pmm_usable_frames(void);
uint64_t pmm_allocated_frames(void);
uint64_t pmm_free_frame_count(void);
uint64_t pmm_total_bytes(void);
uint64_t pmm_free_bytes(void);
uint64_t pmm_bitmap_bytes(void);

#endif /* HELIX_PMM_H */
