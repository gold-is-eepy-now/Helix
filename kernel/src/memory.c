/* memory.c — summarise the bootloader-provided physical memory map.
 *
 * The map is a list of ranges, each tagged with a type (usable RAM, reserved,
 * ACPI, framebuffer, ...). We do not allocate anything yet; this module only
 * reports what exists, which is what `mem` and `sysinfo` need. The actual
 * frame allocator is the next milestone.
 */
#include "memory.h"

#include <stdbool.h>
#include <stddef.h>

#include <limine.h>

static struct limine_memmap_response *g_memmap;

void memory_init(struct limine_memmap_response *memmap) {
    g_memmap = memmap;
}

bool memory_available(void) {
    return g_memmap != NULL && g_memmap->entry_count > 0;
}

uint64_t memory_entry_count(void) {
    return g_memmap ? g_memmap->entry_count : 0;
}

uint64_t memory_total_bytes(void) {
    if (!g_memmap) {
        return 0;
    }
    uint64_t total = 0;
    for (uint64_t i = 0; i < g_memmap->entry_count; i++) {
        total += g_memmap->entries[i]->length;
    }
    return total;
}

uint64_t memory_usable_bytes(void) {
    if (!g_memmap) {
        return 0;
    }
    uint64_t total = 0;
    for (uint64_t i = 0; i < g_memmap->entry_count; i++) {
        if (g_memmap->entries[i]->type == LIMINE_MEMMAP_USABLE) {
            total += g_memmap->entries[i]->length;
        }
    }
    return total;
}

const char *memory_type_name(uint64_t type) {
    switch (type) {
        case LIMINE_MEMMAP_USABLE:                 return "usable";
        case LIMINE_MEMMAP_RESERVED:               return "reserved";
        case LIMINE_MEMMAP_ACPI_RECLAIMABLE:       return "ACPI reclaimable";
        case LIMINE_MEMMAP_ACPI_NVS:               return "ACPI NVS";
        case LIMINE_MEMMAP_BAD_MEMORY:             return "bad memory";
        case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE: return "bootloader reclaimable";
        case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES: return "kernel/modules";
        case LIMINE_MEMMAP_FRAMEBUFFER:            return "framebuffer";
        default:                                   return "unknown";
    }
}

const struct limine_memmap_entry *memory_map_entry(uint64_t index) {
    if (!g_memmap || index >= g_memmap->entry_count) {
        return NULL;
    }
    return g_memmap->entries[index];
}
