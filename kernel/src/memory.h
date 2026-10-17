/* memory.h — the physical memory map handed to us by the bootloader. */
#ifndef HELIX_MEMORY_H
#define HELIX_MEMORY_H

#include <stdbool.h>
#include <stdint.h>

struct limine_memmap_response;
struct limine_memmap_entry;

/* Remember the bootloader's memory map. */
void memory_init(struct limine_memmap_response *memmap);

/* True once a memory map is available. */
bool memory_available(void);

/* Total bytes described by the map. */
uint64_t memory_total_bytes(void);

/* Bytes of type USABLE (free for the kernel to hand out). */
uint64_t memory_usable_bytes(void);

/* Number of entries in the map. */
uint64_t memory_entry_count(void);

/* Human-readable name for a Limine memory-map type. */
const char *memory_type_name(uint64_t type);

/* Access one entry of the map (NULL if out of range). */
const struct limine_memmap_entry *memory_map_entry(uint64_t index);

#endif /* HELIX_MEMORY_H */
