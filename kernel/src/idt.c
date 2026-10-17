/* idt.c — the Interrupt Descriptor Table.
 *
 * The IDT maps each of the 256 interrupt vectors to a handler. We point them
 * all at a default stub first, then install the 32 CPU exception stubs and
 * the 16 hardware IRQ stubs. Exceptions print a readable report and halt;
 * IRQs are dispatched by number and acknowledged.
 */
#include "idt.h"

#include <stddef.h>
#include <stdint.h>

#include "console.h"
#include "io.h"
#include "panic.h"
#include "pic.h"
#include "pit.h"

typedef struct {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} __attribute__((packed)) idt_entry_t;

typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) idt_ptr_t;

static idt_entry_t g_idt[256];
static idt_ptr_t g_idt_ptr;

/* Provided by isr.S. */
extern void (*const interrupt_stubs[])(void);
extern const uint64_t interrupt_stub_count;
extern void isr_default(void);

/* The kernel code selector is whatever the bootloader set up in the GDT.
 * Limine uses 0x28 (not the 0x08 you see in many tutorials), so we read the
 * current CS instead of hardcoding it. */
#define KERNEL_CODE_SELECTOR read_cs()
#define GATE_PRESENT_RING0_INT 0x8Eu /* present, DPL 0, 64-bit interrupt gate */

static void set_gate(uint8_t vector, void (*handler)(void)) {
    uint64_t addr = (uint64_t)handler;
    g_idt[vector].offset_low = (uint16_t)(addr & 0xFFFF);
    g_idt[vector].selector = KERNEL_CODE_SELECTOR;
    g_idt[vector].ist = 0;
    g_idt[vector].type_attr = GATE_PRESENT_RING0_INT;
    g_idt[vector].offset_mid = (uint16_t)((addr >> 16) & 0xFFFF);
    g_idt[vector].offset_high = (uint32_t)((addr >> 32) & 0xFFFFFFFF);
    g_idt[vector].zero = 0;
}

static const char *exception_name(uint64_t vector) {
    switch (vector) {
        case 0:  return "Divide-by-zero";
        case 1:  return "Debug";
        case 2:  return "Non-maskable interrupt";
        case 3:  return "Breakpoint";
        case 4:  return "Overflow";
        case 5:  return "Bound range exceeded";
        case 6:  return "Invalid opcode";
        case 7:  return "Device not available";
        case 8:  return "Double fault";
        case 9:  return "Coprocessor segment overrun";
        case 10: return "Invalid TSS";
        case 11: return "Segment not present";
        case 12: return "Stack-segment fault";
        case 13: return "General protection fault";
        case 14: return "Page fault";
        case 16: return "x87 floating-point";
        case 17: return "Alignment check";
        case 18: return "Machine check";
        case 19: return "SIMD floating-point";
        case 20: return "Virtualization";
        case 21: return "Control protection";
        default: return "Reserved/unknown";
    }
}

void idt_init(void) {
    for (int i = 0; i < 256; i++) {
        set_gate((uint8_t)i, isr_default);
    }
    for (uint64_t i = 0; i < interrupt_stub_count && i < 256; i++) {
        set_gate((uint8_t)i, interrupt_stubs[i]);
    }
    g_idt_ptr.limit = (uint16_t)(sizeof(g_idt) - 1);
    g_idt_ptr.base = (uint64_t)&g_idt;
    __asm__ volatile("lidt %0" : : "m"(g_idt_ptr) : "memory");
}

void isr_handler(registers_t *regs) {
    if (regs->vector < 32) {
        console_set_fg(CONSOLE_BRIGHT_RED);
        kprintf("\n*** CPU EXCEPTION %llu: %s ***\n",
                (unsigned long long)regs->vector, exception_name(regs->vector));
        console_reset_color();
        kprintf("  error code : %p\n", (uintptr_t)regs->error_code);
        kprintf("  RIP        : %p   CS: %p   RFLAGS: %p\n",
                (uintptr_t)regs->rip, (uintptr_t)regs->cs, (uintptr_t)regs->rflags);
        if (regs->vector == 14) {
            kprintf("  fault addr : %p (from CR2)\n", (uintptr_t)read_cr2());
        }
        panic("unhandled CPU exception");
    }

    if (regs->vector < 48) {
        uint8_t irq = (uint8_t)(regs->vector - 32);
        if (irq == 0) {
            pit_tick();
        }
        pic_send_eoi(irq);
        return;
    }

    kprintf("interrupt: unexpected vector %llu\n", (unsigned long long)regs->vector);
}
