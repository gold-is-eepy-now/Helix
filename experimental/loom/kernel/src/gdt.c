/* Global descriptor table and the task state segment.
 *
 * Ring 3 needs its own code and data segments, and the CPU needs a TSS to find the kernel stack
 * when a trap arrives from ring 3. Without the TSS a trap from a Cell would reuse the Cell's stack.
 * Limine leaves its own GDT in place; we replace it so the layout is known. */
#include <stdint.h>
#include <string.h>
#include "arch.h"
#include "loom_kernel.h"

struct __attribute__((packed)) desc_ptr {
    uint16_t limit;
    uint64_t base;
};

struct __attribute__((packed)) tss {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
};

static uint64_t g_gdt[7] __attribute__((aligned(16)));
static struct tss g_tss;

void gdt_load(const struct desc_ptr *p);    /* trap.S */
void tss_load(uint16_t sel);                 /* trap.S */

void tss_set_rsp0(uint64_t top) { g_tss.rsp0 = top; }

void gdt_init(void) {
    memset(g_gdt, 0, sizeof g_gdt);
    g_gdt[1] = 0x00AF9A000000FFFFull;   /* kernel code: 64-bit, DPL 0, present, executable */
    g_gdt[2] = 0x00CF92000000FFFFull;   /* kernel data: DPL 0, present, writable */
    g_gdt[3] = 0x00CFF2000000FFFFull;   /* user data: DPL 3 */
    g_gdt[4] = 0x00AFFA000000FFFFull;   /* user code: DPL 3, 64-bit */

    memset(&g_tss, 0, sizeof g_tss);
    g_tss.iomap_base = sizeof g_tss;    /* no I/O permission bitmap: every port access is refused in ring 3 */

    uint64_t base = (uint64_t)&g_tss;
    uint32_t limit = sizeof g_tss - 1;
    /* 64-bit TSS descriptor: two slots. Type 0x9 (available TSS), present, DPL 0. */
    g_gdt[5] = (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (0x89ull << 40) |
               (((uint64_t)limit >> 16 & 0xF) << 48) | (((base >> 24) & 0xFF) << 56);
    g_gdt[6] = base >> 32;

    struct desc_ptr p = { .limit = sizeof g_gdt - 1, .base = (uint64_t)g_gdt };
    gdt_load(&p);
    tss_load(SEL_TSS);
}
