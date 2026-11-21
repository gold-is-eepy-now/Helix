/* Interrupt descriptor table. Every vector goes to a stub in trap.S. Vector 0x80 is the system
 * call gate, and its DPL is 3 so a Cell may use it; every other gate is DPL 0, so a Cell that
 * tries to fire one gets a general-protection fault instead. */
#include <stdint.h>
#include <string.h>
#include "arch.h"
#include "loom_kernel.h"

struct __attribute__((packed)) idt_entry {
    uint16_t off_lo;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t off_mid;
    uint32_t off_hi;
    uint32_t zero;
};

struct __attribute__((packed)) desc_ptr_idt {
    uint16_t limit;
    uint64_t base;
};

extern const uint64_t isr_table[256];      /* trap.S */
void idt_load(const struct desc_ptr_idt *p);   /* trap.S */

static struct idt_entry g_idt[256] __attribute__((aligned(16)));

static void set_gate(int v, uint64_t handler, uint8_t dpl) {
    g_idt[v].off_lo = handler & 0xFFFF;
    g_idt[v].selector = SEL_KCODE;
    g_idt[v].ist = 0;
    g_idt[v].type_attr = (uint8_t)(0x8E | (dpl << 5));   /* present, interrupt gate */
    g_idt[v].off_mid = (handler >> 16) & 0xFFFF;
    g_idt[v].off_hi = (uint32_t)(handler >> 32);
    g_idt[v].zero = 0;
}

void idt_init(void) {
    memset(g_idt, 0, sizeof g_idt);
    for (int v = 0; v < 256; v++) set_gate(v, isr_table[v], 0);
    set_gate(0x80, isr_table[0x80], 3);

    struct desc_ptr_idt p = { .limit = sizeof g_idt - 1, .base = (uint64_t)g_idt };
    idt_load(&p);
}
