/* idt.h — the Interrupt Descriptor Table and CPU exception handling. */
#ifndef HELIX_IDT_H
#define HELIX_IDT_H

#include <stdbool.h>
#include <stdint.h>

/* The interrupt frame as laid out by isr.S. */
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector;
    uint64_t error_code;
    uint64_t rip, cs, rflags;
    /* In 64-bit mode the CPU also saves/restores the stack pair. */
    uint64_t rsp, ss;
} registers_t;

/* Build and load the IDT. */
void idt_init(void);

/* Arm a one-shot recovery for a page fault at an exact CR2 address. */
bool idt_expect_page_fault(uint64_t address, uint64_t recovery_rip);
void idt_clear_page_fault_expectation(void);
bool idt_expected_page_fault_caught(void);
uint64_t idt_expected_page_fault_error(void);

/* Called from isr.S with a saved frame; may return another thread's frame. */
registers_t *isr_handler(registers_t *regs);

/* DPL-3 software interrupt used by the first user-mode exit diagnostic. */
void idt_set_user_vector(void (*handler)(void));

#endif /* HELIX_IDT_H */
