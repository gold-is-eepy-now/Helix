/* gdt.c — Global Descriptor Table with ring-3 user segments.
 *
 * We append user code/data segment entries to the existing Limine-provided
 * GDT. This is safe because:
 *   - All existing kernel segments remain valid (CS/DS/SS unchanged)
 *   - No far jump is needed (kernel CS/DS/SS cache is still valid)
 *   - The new user segments are only used when explicitly loaded by
 *     user-space code via iretq (which pushes the user selectors onto the
 *     current stack)
 *
 * A TSS is NOT installed here. The syscall gate uses `int $0x80`, which
 * pushes the ring-3 frame (user RIP/CS/RFLAGS/SS/RSP) onto the current
 * kernel stack. No automatic stack switch is needed. A TSS would only
 * be required for automatic RSP0 switching on exceptions/interrupts from
 * ring 3; that is deferred until user-space exception handling is wired up.
 */

#include "gdt.h"

#include <stddef.h>
#include <stdint.h>

#include "io.h"
#include "kstring.h"

/* 64-bit GDT entry (code/data). */
typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed)) gdt_entry_t;

/* GDTR structure for SGDT/LGDT. */
typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) gdtr_t;

/* Maximum GDT entries we support. */
#define GDT_MAX_ENTRIES 16u

static gdt_entry_t g_gdt[GDT_MAX_ENTRIES];

/* 64-bit task state segment. Only rsp0 matters: the CPU loads it as the kernel
 * stack pointer when an interrupt or int $0x80 arrives from ring 3. */
typedef struct {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed)) tss_t;

static tss_t g_tss;
static uint16_t g_tss_selector;
static uint16_t g_user_cs;
static uint16_t g_user_ss;

/* Access byte flags for code/data segments. */
#define ACC_PRESENT     (1u << 7)
#define ACC_DPL(x)      ((x & 3) << 5)
#define ACC_CODE        (1u << 3)  /* 1 = code, 0 = data */
#define ACC_READABLE    (1u << 1)  /* code: readable; data: writable */
#define ACC_ACCESSED    (1u << 0)
#define ACC_CODE_DATA   (1u << 4)  /* S bit: code/data segment, not a system one */

/* Granularity byte flags. */
#define GRAN_4K         (1u << 7)
#define GRAN_64BIT      (1u << 5)

/* Set a code-segment descriptor. */
static void gdt_set_code(gdt_entry_t *e, uint64_t base, uint64_t limit, uint8_t dpl) {
    e->limit_low = (uint16_t)(limit & 0xFFFF);
    e->base_low = (uint16_t)(base & 0xFFFF);
    e->base_mid = (uint8_t)((base >> 16) & 0xFF);
    e->access = ACC_PRESENT | ACC_DPL(dpl) | ACC_CODE_DATA | ACC_CODE | ACC_READABLE | ACC_ACCESSED;
    e->granularity = GRAN_4K | GRAN_64BIT | (uint8_t)((limit >> 16) & 0xF);
    e->base_high = (uint8_t)((base >> 24) & 0xFF);
}

/* Set a data-segment descriptor. */
static void gdt_set_data(gdt_entry_t *e, uint64_t base, uint64_t limit, uint8_t dpl) {
    e->limit_low = (uint16_t)(limit & 0xFFFF);
    e->base_low = (uint16_t)(base & 0xFFFF);
    e->base_mid = (uint8_t)((base >> 16) & 0xFF);
    e->access = ACC_PRESENT | ACC_DPL(dpl) | ACC_CODE_DATA | ACC_READABLE | ACC_ACCESSED;
    e->granularity = GRAN_4K | (uint8_t)((limit >> 16) & 0xF);
    e->base_high = (uint8_t)((base >> 24) & 0xFF);
}

/* Initialize the GDT with user code/data segments.
 *
 * Reads the current GDT (set up by Limine), appends user code/data entries,
 * and reloads the GDTR. No far jump is needed because the kernel's CS/DS/SS
 * selectors are unchanged.
 */
void gdt_init(void) {
    gdtr_t gdtr;
    __asm__ volatile("sgdt %0" : "=m"(gdtr));

    int num_entries = (int)(gdtr.limit + 1) / 8;
    if (num_entries > GDT_MAX_ENTRIES - 4) num_entries = GDT_MAX_ENTRIES - 4;

    /* Copy existing entries. */
    memcpy(g_gdt, (const void *)gdtr.base, (size_t)num_entries * sizeof(gdt_entry_t));

    /* Append user code and user data entries. */
    gdt_set_code(&g_gdt[num_entries], 0, 0xFFFFF, 3);   /* user code, ring 3 */
    g_user_cs = (uint16_t)((num_entries * 8) | 3);
    g_user_ss = (uint16_t)(((num_entries + 1) * 8) | 3);
    gdt_set_data(&g_gdt[num_entries + 1], 0, 0xFFFFF, 3); /* user data, ring 3 */

    /* A 64-bit TSS descriptor is 16 bytes: two slots, the second holds the
     * upper 32 bits of the base. */
    uint64_t tss_base = (uint64_t)(uintptr_t)&g_tss;
    uint32_t tss_limit = sizeof(tss_t) - 1u;
    gdt_entry_t *tss_lo = &g_gdt[num_entries + 2];
    tss_lo->limit_low = (uint16_t)(tss_limit & 0xFFFF);
    tss_lo->base_low = (uint16_t)(tss_base & 0xFFFF);
    tss_lo->base_mid = (uint8_t)((tss_base >> 16) & 0xFF);
    tss_lo->access = 0x89;                     /* present, DPL 0, 64-bit available TSS */
    tss_lo->granularity = (uint8_t)((tss_limit >> 16) & 0xF);
    tss_lo->base_high = (uint8_t)((tss_base >> 24) & 0xFF);
    *(uint64_t *)&g_gdt[num_entries + 3] = tss_base >> 32;
    g_tss.iomap_base = (uint16_t)sizeof(tss_t);
    g_tss_selector = (uint16_t)((num_entries + 2) * 8);

    /* Load the modified GDT, then the task register. */
    gdtr_t new_gdtr = {
        .limit = (uint16_t)((num_entries + 4) * sizeof(gdt_entry_t) - 1),
        .base = (uint64_t)&g_gdt
    };

    __asm__ volatile("lgdt %0" : : "m"(new_gdtr) : "memory");
    __asm__ volatile("ltr %0" : : "r"(g_tss_selector));
}

uint16_t gdt_user_code_selector(void) {
    return g_user_cs;
}

uint16_t gdt_user_data_selector(void) {
    return g_user_ss;
}

/* Kernel stack the CPU switches to when ring-3 code traps into the kernel. */
void gdt_set_rsp0(uint64_t rsp0) {
    g_tss.rsp0 = rsp0;
}

/* Read it back: a nested launch restores the caller's kernel stack. */
uint64_t gdt_get_rsp0(void) {
    return g_tss.rsp0;
}

/* Not used when TSS is not installed. */
void gdt_load_tss(uint64_t tss_phys, uint64_t hhdm_offset) {
    (void)tss_phys;
    (void)hhdm_offset;
}