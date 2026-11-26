/* gdt.c — a flat 4 GiB code and data segment, both ring 0.
 *
 * Why our own GDT: the Multiboot spec says the segment layout the loader leaves behind is
 * not ours to rely on. We observed exactly that: an interrupt through GRUB's selector 0x08
 * raised a general-protection fault. A three-entry table we control removes the dependency.
 *
 * Descriptor bits (8 bytes each): base and limit split across fields; access byte 0x9A is
 * present + ring 0 + code, readable; 0x92 is present + ring 0 + data, writable. The
 * granularity byte 0xCF means 4 KiB units and 32-bit operands, so limit 0xFFFFF covers 4 GiB. */
#include "gdt.h"

#include <stdint.h>

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_mid;
    uint8_t access;
    uint8_t granularity;
    uint8_t base_high;
} __attribute__((packed));

struct gdt_pointer {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

static struct gdt_entry g_gdt[3] = {
    { 0, 0, 0, 0, 0, 0 },              /* entry 0: null, required by the CPU */
    { 0xFFFF, 0, 0, 0x9A, 0xCF, 0 },   /* selector 0x08: code */
    { 0xFFFF, 0, 0, 0x92, 0xCF, 0 },   /* selector 0x10: data */
};

void gdt_init(void) {
    struct gdt_pointer ptr = {
        .limit = (uint16_t)(sizeof(g_gdt) - 1),
        .base = (uint32_t)(uintptr_t)g_gdt,
    };
    __asm__ volatile("lgdt %0" : : "m"(ptr));
    /* A far jump is the only way to load CS; then load the data selector into the others. */
    __asm__ volatile(
        "ljmp $0x08, $1f\n"
        "1:\n"
        "mov $0x10, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        "mov %%ax, %%ss\n"
        : : : "eax", "memory");
}
