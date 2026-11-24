/* isr.S — interrupt and exception stubs (GAS syntax, .S so it is preprocessed).
 *
 * When the CPU takes an interrupt it pushes an "interrupt frame" on the
 * stack: RIP, CS, RFLAGS (and SS:RSP if the privilege level changed). For a
 * few exceptions it also pushes an error code; for the rest it does not. To
 * keep the frame shape uniform, the stubs push a dummy error code when there
 * is none, then the vector number, then fall into `isr_common`.
 *
 * `isr_common` saves all the general-purpose registers, passes a pointer to
 * the whole frame to the C handler `isr_handler`, restores everything, and
 * returns with `iretq`.
 *
 * The resulting stack frame, from the stack pointer upward, is:
 *
 *   r15 r14 r13 r12 r11 r10 r9 r8 rbp rdi rsi rdx rcx rbx rax
 *   vector  error_code  rip  cs  rflags
 *
 * which is exactly `registers_t` in idt.h.
 */

.macro ISR_NOERR num
.global isr\num
isr\num:
    pushq $0
    pushq $\num
    jmp isr_common
.endm

.macro ISR_ERR num
.global isr\num
isr\num:
    pushq $\num
    jmp isr_common
.endm

.section .text

/* CPU exceptions 0-31. Vectors 8, 10-14, 17, 21 and 29-30 push an error code. */
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
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31

/* Hardware IRQs 0-15, remapped to vectors 32-47. */
.macro IRQ num, vec
.global irq\num
irq\num:
    pushq $0
    pushq $\vec
    jmp isr_common
.endm

IRQ 0, 32
IRQ 1, 33
IRQ 2, 34
IRQ 3, 35
IRQ 4, 36
IRQ 5, 37
IRQ 6, 38
IRQ 7, 39
IRQ 8, 40
IRQ 9, 41
IRQ 10, 42
IRQ 11, 43
IRQ 12, 44
IRQ 13, 45
IRQ 14, 46
IRQ 15, 47

/* Everything else lands here. */
.global isr_default
isr_default:
    pushq $0
    pushq $255
    jmp isr_common

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

    movq %rsp, %rdi        /* first argument: registers_t * */
    cld
    call isr_handler

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

    addq $16, %rsp         /* drop vector and error code */
    iretq

/* A table of stub addresses, in vector order, for the C code to install. */
.section .rodata
.align 8
.global interrupt_stubs
interrupt_stubs:
    .quad isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7
    .quad isr8, isr9, isr10, isr11, isr12, isr13, isr14, isr15
    .quad isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23
    .quad isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31
    .quad irq0, irq1, irq2, irq3, irq4, irq5, irq6, irq7
    .quad irq8, irq9, irq10, irq11, irq12, irq13, irq14, irq15

.global interrupt_stub_count
interrupt_stub_count:
    .quad 48
