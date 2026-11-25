/* main.c — i686 kernel, stage I2a: check the Multiboot magic, build the frame list from the
 * Multiboot memory map, run the frame allocator self-test, print over COM1, halt.
 *
 * Why COM1 first: serial is the simplest output that works before any framebuffer or
 * memory code exists, and QEMU's -serial stdio shows it directly. */
#include <stddef.h>
#include <stdint.h>

#include "heap.h"
#include "paging.h"
#include "pmm.h"

bool mm_selftest(void);
bool paging_selftest(void);
bool heap_selftest(void);

#define COM1 0x3F8
#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002u

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

/* Wait until the transmit holding register is empty (line status bit 5), then send. */
static void serial_putc(char c) {
    while ((inb(COM1 + 5) & 0x20) == 0) { }
    outb(COM1, (uint8_t)c);
}

static void serial_puts(const char *s) {
    while (*s) {
        if (*s == '\n') serial_putc('\r');
        serial_putc(*s++);
    }
}

/* Multiboot 1 information block (only the fields we read). flags bit 6 means the memory
 * map is present. */
struct multiboot_info {
    uint32_t flags;
    uint32_t mem_lower;
    uint32_t mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;
    uint32_t mmap_addr;
} __attribute__((packed));

/* One memory map entry. `size` counts the bytes after the size field itself. */
struct multiboot_mmap_entry {
    uint32_t size;
    uint64_t addr;
    uint64_t len;
    uint32_t type;      /* 1 = available RAM */
} __attribute__((packed));

#define MULTIBOOT_MMAP_PRESENT (1u << 6)
#define MULTIBOOT_MMAP_AVAILABLE 1u
#define FOUR_GIB 0x100000000ULL

extern char __kernel_start[];
extern char __kernel_end[];

static struct pmm_region g_regions[64];
static size_t g_region_count;

/* Add one usable span, minus the kernel image, minus anything at or above 4 GiB (the i686
 * stage uses an identity map, so physical addresses must fit in a 32-bit pointer). */
static void add_usable(uint64_t base, uint64_t length) {
    const uint64_t kstart = (uint64_t)(uintptr_t)__kernel_start;
    const uint64_t kend = (uint64_t)(uintptr_t)__kernel_end;
    if (base >= FOUR_GIB || length == 0) {
        return;
    }
    if (length > FOUR_GIB - base) {
        length = FOUR_GIB - base;
    }
    uint64_t end = base + length;
    if (end <= kstart || base >= kend) {
        g_regions[g_region_count++] = (struct pmm_region){ .base = base, .length = length, .usable = true };
        return;
    }
    /* Overlaps the kernel image: keep the part below it and the part above it. */
    if (base < kstart) {
        g_regions[g_region_count++] = (struct pmm_region){ .base = base, .length = kstart - base, .usable = true };
    }
    if (end > kend) {
        g_regions[g_region_count++] = (struct pmm_region){ .base = kend, .length = end - kend, .usable = true };
    }
}

static void build_regions(const struct multiboot_info *mbi) {
    if ((mbi->flags & MULTIBOOT_MMAP_PRESENT) == 0) {
        return; /* no map: the allocator stays empty and the self-test reports it */
    }
    uint32_t offset = 0;
    while (offset < mbi->mmap_length) {
        const struct multiboot_mmap_entry *e =
            (const struct multiboot_mmap_entry *)(uintptr_t)(mbi->mmap_addr + offset);
        if (e->type == MULTIBOOT_MMAP_AVAILABLE && g_region_count + 2 <= 64) {
            add_usable(e->addr, e->len);
        }
        offset += e->size + 4;
    }
}

void kmain(uint32_t multiboot_magic, uint32_t multiboot_info) {
    if (multiboot_magic != MULTIBOOT_BOOTLOADER_MAGIC) {
        serial_puts("HELIX i686: not loaded by a Multiboot loader\n");
        for (;;) { __asm__ volatile("cli; hlt"); }
    }
    serial_puts("HELIX OS i686\n");

    build_regions((const struct multiboot_info *)(uintptr_t)multiboot_info);
    pmm_init(g_regions, g_region_count, 0);   /* identity map: phys == virt */
    serial_puts(mm_selftest() ? "HELIX i686 mm: PASS\n" : "HELIX i686 mm: FAIL\n");

    /* Identity-map RAM up to the highest usable byte, then run the paging and heap tests. */
    uint64_t top = 0;
    for (size_t i = 0; i < g_region_count; i++) {
        uint64_t end = g_regions[i].base + g_regions[i].length;
        if (end > top) top = end;
    }
    serial_puts(paging_init(top) ? "HELIX i686 paging init: OK\n" : "HELIX i686 paging init: FAIL\n");
    serial_puts(paging_selftest() ? "HELIX i686 paging: PASS\n" : "HELIX i686 paging: FAIL\n");
    heap_init(0);                            /* identity map: phys + 0 */
    serial_puts(heap_selftest() ? "HELIX i686 heap: PASS\n" : "HELIX i686 heap: FAIL\n");

    serial_puts("Boot successful.\n");
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}
