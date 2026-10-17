/* idt.h — the Interrupt Descriptor Table and CPU exception handling. */
#ifndef HELIX_IDT_H
#define HELIX_IDT_H

#include <stdint.h>

/* The interrupt frame as laid out by isr.S. */
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector;
    uint64_t error_code;
    uint64_t rip, cs, rflags;
} registers_t;

/* Build and load the IDT. */
void idt_init(void);

/* Called from isr.S with a pointer to the saved frame. */
void isr_handler(registers_t *regs);

#endif /* HELIX_IDT_H */
