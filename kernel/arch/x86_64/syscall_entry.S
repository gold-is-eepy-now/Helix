/* syscall_entry.S — syscall entry and exit stub.
 *
 * Entry: when a user thread executes `int $0x80` (0xCD 0x80), control
 * transfers to ring 0 via a software interrupt trap (vector 0x80 in the IDT).
 * The stub (installed as the IDT handler) preserves the user frame, jumps to
 * `syscall_dispatch` in C, and on return re-enters user mode with iretq.
 *
 * Register usage:
 *   rax  = syscall number (also returned to caller by default)
 *   rdi  = arg1
 *   rsi  = arg2
 *   rdx  = arg3
 *   r10  = arg4
 *   r8   = arg5
 *   r9   = arg6
 * All general-purpose registers are preserved across the call.
 */

.section .text

/* The IDT handler for vector 0x80 (software interrupt). We install it in
 * idt_init() so its address is known at link time. */
.global syscall_interrupt_handler
syscall_interrupt_handler:
    /* The CPU has pushed the hardware interrupt frame on entry (ring 3 -> 0):
     *   [rsp+0]  user RIP
     *   [rsp+8]  user CS
     *   [rsp+16] user RFLAGS
     *   [rsp+24] user SS
     *   [rsp+32] user RSP
     * We save all general-purpose registers, then pass a pointer to the
     * full frame (minus the hardware-pushed bits) to the C dispatcher. */
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
    call syscall_dispatch

    /* Restore registers in reverse order. */
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

    /* iretq pops user RIP, CS, RFLAGS, SS, and RSP from the stack and
     * returns to user mode. */
    iretq