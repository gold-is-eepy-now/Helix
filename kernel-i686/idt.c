/* idt.c — the IDT itself and the C-side dispatcher the assembly stubs call.
 *
 * Each IDT entry is 8 bytes: a 32-bit handler address split in two halves, a code segment
 * selector and attribute bits. The "interrupt gate" type (0x8E) makes the CPU clear IF on
 * entry, so a handler is not interrupted by the timer until it returns (iret restores IF). */
#include "idt.h"

#include "pic.h"
#include "serial.h"
#include "timer.h"

#define IDT_ENTRIES 256
#define GATE_INTERRUPT_PRESENT_RING0 0x8E
#define KERNEL_CODE_SELECTOR 0x08     /* Multiboot/GRUB leaves a flat code segment here */

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t zero;
    uint8_t type_attr;
    uint16_t offset_high;
} __attribute__((packed));

struct idt_pointer {
    uint16_t limit;               /* size in bytes minus one */
    uint32_t base;
} __attribute__((packed));

extern const uint32_t isr_stub_table[];

static struct idt_entry g_idt[IDT_ENTRIES] __attribute__((aligned(8)));
static uint32_t g_breakpoints;

static void set_gate(unsigned vector, uint32_t handler) {
    g_idt[vector].offset_low = (uint16_t)(handler & 0xFFFF);
    g_idt[vector].selector = KERNEL_CODE_SELECTOR;
    g_idt[vector].zero = 0;
    g_idt[vector].type_attr = GATE_INTERRUPT_PRESENT_RING0;
    g_idt[vector].offset_high = (uint16_t)((handler >> 16) & 0xFFFF);
}

void idt_init(void) {
    for (unsigned v = 0; v < 33; v++) {
        set_gate(v, isr_stub_table[v]);
    }
    /* Vectors 33..255 keep an all-zero gate (not present). An interrupt there faults. */
    struct idt_pointer ptr = {
        .limit = (uint16_t)(sizeof(g_idt) - 1),
        .base = (uint32_t)(uintptr_t)g_idt,
    };
    __asm__ volatile("lidt %0" : : "m"(ptr));
}

uint32_t idt_breakpoint_count(void) {
    return g_breakpoints;
}

void isr_dispatch(struct intr_frame *f) {
    if (f->vector == VECTOR_IRQ0_TIMER) {
        timer_tick();
        pic_eoi(0);                  /* tell the PIC the interrupt is handled */
        return;
    }
    if (f->vector == VECTOR_BREAKPOINT) {
        g_breakpoints++;             /* int3 is a trap: eip already points past it */
        return;
    }
    /* Any other vector is an exception we did not expect. Report it and stop: continuing
     * would run on a broken state. */
    serial_puts("HELIX i686 exception vector=");
    serial_put_hex32(f->vector);
    serial_puts(" error=");
    serial_put_hex32(f->error_code);
    serial_puts(" eip=");
    serial_put_hex32(f->eip);
    serial_puts("\n");
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}
