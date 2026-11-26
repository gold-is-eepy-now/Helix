/* isr.S — interrupt and exception entry stubs for the i686 IDT.
 *
 * Why stubs: the CPU jumps to a fixed address with no arguments. Each stub normalises the
 * stack (CPUs push an error code for some exceptions only) and records the vector number,
 * then jumps to one common routine that saves registers and calls C.
 *
 * Stack after the stubs (low to high addresses), as isr_dispatch sees it through struct
 * intr_frame in idt.h:  edi esi ebp esp ebx edx ecx eax | vector error_code | eip cs eflags */

    .section .text

    .macro ISR_NOERR n
    .global isr\n
isr\n:
    pushl $0                    /* fake error code, so every frame has the same shape */
    pushl $\n                   /* vector number */
    jmp isr_common
    .endm

    .macro ISR_ERR n
    .global isr\n
isr\n:
    pushl $\n                   /* the CPU already pushed the real error code */
    jmp isr_common
    .endm

    ISR_NOERR 0
    ISR_NOERR 1
    ISR_NOERR 2
    ISR_NOERR 3
    ISR_NOERR 4
    ISR_NOERR 5
    ISR_NOERR 6
    ISR_NOERR 7
    ISR_ERR   8
    ISR_NOERR 9
    ISR_ERR   10
    ISR_ERR   11
    ISR_ERR   12
    ISR_ERR   13
    ISR_ERR   14
    ISR_NOERR 15
    ISR_NOERR 16
    ISR_ERR   17
    ISR_NOERR 18
    ISR_NOERR 19
    ISR_NOERR 20
    ISR_NOERR 21
    ISR_NOERR 22
    ISR_NOERR 23
    ISR_NOERR 24
    ISR_NOERR 25
    ISR_NOERR 26
    ISR_NOERR 27
    ISR_NOERR 28
    ISR_NOERR 29
    ISR_NOERR 30
    ISR_NOERR 31
    ISR_NOERR 32                /* IRQ0 after the PIC remap: the PIT timer */

isr_common:
    pusha                       /* saves eax, ecx, edx, ebx, esp, ebp, esi, edi */
    pushl %esp                  /* argument: pointer to the frame we just built */
    call isr_dispatch
    addl $4, %esp
    popa
    addl $8, %esp               /* drop the vector number and the error code */
    iret                        /* pops eip, cs, eflags; resumes the interrupted code */

/* Table of the 33 stubs, read by idt_init(). Order matches vector numbers 0..32. */
    .section .rodata
    .global isr_stub_table
    .align 4
isr_stub_table:
    .long isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7
    .long isr8, isr9, isr10, isr11, isr12, isr13, isr14, isr15
    .long isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23
    .long isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31
    .long isr32

    .section .note.GNU-stack,"",@progbits
