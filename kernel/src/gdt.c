/* gdt.c — minimal GDT and 64-bit TSS for ring-3 entry and interrupt return. */
#include "gdt.h"

#include <stddef.h>
#include "kstring.h"

typedef struct {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed)) tss_t;

typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) gdtr_t;

static uint64_t g_gdt[7] __attribute__((aligned(16)));
static tss_t g_tss __attribute__((aligned(16)));
static gdtr_t g_gdtr;
static bool g_ready;

extern void gdt_load(const gdtr_t *gdtr);

static void set_tss_descriptor(uint64_t *low, uint64_t base, uint32_t limit) {
    *low = ((uint64_t)(limit & 0xFFFFu)) |
           ((base & 0xFFFFFFu) << 16) |
           (0x89ull << 40) |
           ((uint64_t)((limit >> 16) & 0xFu) << 48) |
           (((base >> 24) & 0xFFu) << 56);
    low[1] = base >> 32;
}

bool gdt_init(uint64_t initial_rsp0) {
    if (g_ready || initial_rsp0 == 0) return false;
    memset(g_gdt, 0, sizeof(g_gdt));
    memset(&g_tss, 0, sizeof(g_tss));

    /* Flat long-mode code/data segments. User descriptors have DPL 3. */
    g_gdt[1] = 0x00AF9A000000FFFFull; /* kernel code */
    g_gdt[2] = 0x00CF92000000FFFFull; /* kernel data */
    g_gdt[3] = 0x00CFF2000000FFFFull; /* user data */
    g_gdt[4] = 0x00AFFA000000FFFFull; /* user code */
    g_tss.rsp0 = initial_rsp0;
    g_tss.iomap_base = sizeof(g_tss);
    set_tss_descriptor(&g_gdt[5], (uint64_t)(uintptr_t)&g_tss,
                       sizeof(g_tss) - 1);

    g_gdtr.limit = (uint16_t)(sizeof(g_gdt) - 1);
    g_gdtr.base = (uint64_t)(uintptr_t)g_gdt;
    gdt_load(&g_gdtr);
    g_ready = true;
    return true;
}

bool gdt_set_rsp0(uint64_t rsp0) {
    if (!g_ready || rsp0 == 0) return false;
    g_tss.rsp0 = rsp0;
    return true;
}
