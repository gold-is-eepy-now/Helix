/* x86_64 CPU structures used by the LOOM kernel: descriptor tables, traps and Cell entry. */
#ifndef LOOM_ARCH_H
#define LOOM_ARCH_H

#include <stdint.h>

/* Selectors (see gdt.c for the table). RPL 3 is or-ed into the user ones. */
#define SEL_KCODE  0x08
#define SEL_KDATA  0x10
#define SEL_UDATA  0x1B      /* 0x18 | 3 */
#define SEL_UCODE  0x23      /* 0x20 | 3 */
#define SEL_TSS    0x28

/* What the common trap stub builds on the stack, lowest address first. */
struct trap_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error;
    uint64_t rip, cs, rflags, rsp, ss;   /* pushed by the CPU */
};

/* A Cell's run record. The field order is fixed: trap.S reads these offsets. */
struct cell_ctx {
    uint64_t saved_rsp;      /* 0:  kernel stack pointer to resume at (set by cell_enter) */
    uint64_t user_cr3;       /* 8:  the Cell's address space */
    uint64_t entry;          /* 16: first instruction */
    uint64_t user_rsp;       /* 24: top of the Cell's stack */
    uint64_t kernel_cr3;     /* 32: restored on the way out (set by cell_enter) */
};

void gdt_init(void);
void idt_init(void);
void tss_set_rsp0(uint64_t top);

/* Run a Cell to completion (exit or fault). Sets the trap stack first. Returns its status. */
int64_t cell_run(struct cell_ctx *c);
struct cell_ctx *cell_current(void);

/* Enter a Cell in ring 3. Returns the status the Cell passed to cell_leave(). */
int64_t cell_enter(struct cell_ctx *c);
/* Leave the Cell and return `status` from the matching cell_enter. Does not return to the Cell. */
__attribute__((noreturn)) void cell_leave(struct cell_ctx *c, int64_t status);

#endif
