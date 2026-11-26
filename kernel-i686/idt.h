/* idt.h — the Interrupt Descriptor Table: which code runs for each interrupt vector. */
#ifndef HELIX_I686_IDT_H
#define HELIX_I686_IDT_H

#include <stdint.h>

/* What the stubs in isr.S leave on the stack. Field order is the reverse of the push order. */
struct intr_frame {
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;   /* pusha, lowest address first */
    uint32_t vector;
    uint32_t error_code;                                      /* 0 if the CPU sent none */
    uint32_t eip, cs, eflags;                                 /* pushed by the CPU */
} __attribute__((packed));

void idt_init(void);
void isr_dispatch(struct intr_frame *frame);
uint32_t idt_breakpoint_count(void);

#define VECTOR_BREAKPOINT 3
#define VECTOR_IRQ0_TIMER 32

#endif /* HELIX_I686_IDT_H */
