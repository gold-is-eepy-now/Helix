/* thread_switch.S — context switch for the round-robin scheduler.
 *
 * A thread context stores every general-purpose register plus rsp, rip and
 * rflags (see thread_context_t). Resuming therefore does not rely on `ret`
 * finding a return address on the restored stack: the switch pushes the
 * saved rip and rflags onto the incoming stack itself, restores the
 * registers, pops rflags and returns into the saved rip. That is the same
 * state an `iretq` would produce, which is why a thread preempted inside the
 * timer ISR can be resumed here without ever returning through `iretq`.
 *
 * Two entry points share the load phase:
 *   thread_switch_to(out_ctx, in_ctx)  — yield path: saves the callee-saved
 *                                        registers and the resume rip/rsp/
 *                                        rflags of the caller, then loads.
 *   thread_switch_load(in_ctx)         — preemption path: the caller
 *                                        (thread_tick) has already stored the
 *                                        full context from the interrupt
 *                                        frame, so only the load runs.
 */
.section .text

.global thread_switch_to
.type thread_switch_to, @function
thread_switch_to:
    movq %rbx, 72(%rdi)
    movq %rbp, 80(%rdi)
    movq %r12, 88(%rdi)
    movq %r13, 96(%rdi)
    movq %r14, 104(%rdi)
    movq %r15, 112(%rdi)
    pushfq
    popq %rax
    movq %rax, 136(%rdi)      /* rflags */
    movq (%rsp), %rax
    movq %rax, 128(%rdi)      /* rip: return into the yielding caller */
    leaq 8(%rsp), %rax
    movq %rax, 120(%rdi)      /* rsp: caller's stack once we "returned" */
    movq %rsi, %rdi           /* fall into the shared load phase */
.size thread_switch_to, .-thread_switch_to

.global thread_switch_load
.type thread_switch_load, @function
thread_switch_load:           /* rdi = in_ctx */
    movq %rdi, %r12
    movq 120(%r12), %rsp      /* incoming thread's stack */
    subq $16, %rsp            /* build the resume slot below it */
    movq 136(%r12), %rax
    movq %rax, (%rsp)         /* rflags slot */
    movq 128(%r12), %rax
    movq %rax, 8(%rsp)        /* rip slot */
    movq 8(%r12), %rcx
    movq 16(%r12), %rdx
    movq 24(%r12), %rsi
    movq 32(%r12), %rdi
    movq 40(%r12), %r8
    movq 48(%r12), %r9
    movq 56(%r12), %r10
    movq 64(%r12), %r11
    movq 72(%r12), %rbx
    movq 80(%r12), %rbp
    movq 96(%r12), %r13
    movq 104(%r12), %r14
    movq 112(%r12), %r15
    popfq
    movq (%r12), %rax         /* incoming rax: rax is also the scratch above */
    movq 88(%r12), %r12       /* incoming r12: last use of the ctx base */
    ret                       /* pops the rip slot we pushed */
.size thread_switch_load, .-thread_switch_load
