/* Low-level entry and exit: trap stubs for all 256 vectors, the common trap path, the GDT/IDT/TSS
 * loaders, and cell_enter / cell_leave, which move the CPU between the kernel and a Cell.
 *
 * GNU as, AT&T syntax. The CPU pushes an error code for some vectors and not others; the stubs
 * make the trap frame the same size for all of them (see struct trap_frame in arch.h). */

    .altmacro

    .macro isr_stub n
    .globl isr_stub_\n
isr_stub_\n:
    .if (\n == 8) || ((\n >= 10) && (\n <= 14)) || (\n == 17) || (\n == 21) || (\n == 29) || (\n == 30)
    pushq $\n                 /* the CPU already pushed an error code */
    .else
    pushq $0                  /* no error code: push a zero so the frame has the same shape */
    pushq $\n
    .endif
    jmp isr_common
    .endm

    .set i, 0
    .rept 256
    isr_stub %i
    .set i, i + 1
    .endr

    .macro tbl_entry n
    .quad isr_stub_\n
    .endm

    .section .rodata
    .globl isr_table
    .align 8
isr_table:
    .set i, 0
    .rept 256
    tbl_entry %i
    .set i, i + 1
    .endr
    .noaltmacro

    .text

isr_common:
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, %rdi           /* argument: the trap frame */
    movq %rsp, %rbx           /* rbx is callee-saved; it survives the call */
    andq $-16, %rsp           /* the C ABI wants a 16-byte aligned stack at the call */
    call trap_dispatch
    movq %rbx, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    addq $16, %rsp            /* drop the vector and error code */
    iretq

/* void gdt_load(const struct desc_ptr *p): load the GDT, then reload the code and data segments. */
    .globl gdt_load
gdt_load:
    lgdt (%rdi)
    pushq $0x08
    leaq 1f(%rip), %rax
    pushq %rax
    lretq                     /* far return reloads CS */
1:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    xorl %eax, %eax
    movw %ax, %fs
    movw %ax, %gs
    ret

    .globl tss_load
tss_load:
    ltr %di
    ret

    .globl idt_load
idt_load:
    lidt (%rdi)
    ret

/* int64_t cell_enter(struct cell_ctx *c)
 * Saves the kernel's callee-saved registers and stack, switches to the Cell's address space,
 * and drops to ring 3 with iretq. Returns when the Cell calls cell_leave. */
    .globl cell_enter
cell_enter:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, 0(%rdi)        /* saved_rsp: where cell_leave resumes */
    movq %cr3, %rax
    movq %rax, 32(%rdi)       /* kernel_cr3 */
    movq 8(%rdi), %rax
    movq %rax, %cr3           /* the Cell's address space */
    pushq $0x1B          /* iret frame: ss, rsp, rflags, cs, rip */
    pushq 24(%rdi)
    pushq $0x2                /* rflags: bit 1 is always set; interrupts stay off */
    pushq $0x23
    pushq 16(%rdi)
    iretq

/* void cell_leave(struct cell_ctx *c, int64_t status)
 * Undoes cell_enter: the kernel's address space and stack come back, and the callee-saved
 * registers are restored, so cell_enter's caller sees `status` as its return value. */
    .globl cell_leave
cell_leave:
    movq 32(%rdi), %rax
    movq %rax, %cr3
    movq 0(%rdi), %rsp
    movq %rsi, %rax
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret

    .section .note.GNU-stack,"",@progbits
