/* pmm.h — physical memory manager: a bitmap of 4 KiB frames.
 *
 * The bootloader hands us a map of physical memory tagged by type. The PMM
 * turns the USABLE parts of that map into a bitmap of frames and hands them
 * out one (or several, contiguous) at a time. Everything above this — the
 * heap, page tables, user-space memory — is built from frames.
 */
#ifndef HELIX_PMM_H
#define HELIX_PMM_H

#include <stddef.h>
#include <stdint.h>

struct limine_memmap_response;

#define PAGE_SIZE 4096u

/* Build the frame bitmap from the bootloader memory map. */
void pmm_init(struct limine_memmap_response *memmap);

/* Allocate one frame; returns its physical address, or 0 if out of memory. */
uintptr_t pmm_alloc_frame(void);

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
