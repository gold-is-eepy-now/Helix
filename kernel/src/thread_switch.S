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

/* thread_save_current(out_ctx = rdi): store the full context of the calling
 * thread into *out_ctx and return normally. The caller resumes right after
 * the call; no CPU switch happens here. Blocking IPC uses it to park a
 * thread's state before marking itself WAITING.
 *
 * The context pointer arrives in rdi (System V ABI) and would be clobbered
 * by the pushq %rdi used later, so the live rdi value is stored into the
 * context before the push/pop save sequence begins.
 *
 * Getting `rip` out without disturbing the saved stack pointer: the `call`
 * below pushes the address of the next instruction; the caller's real return
 * address sits 8 bytes above it. */
.global thread_save_current
.type thread_save_current, @function
thread_save_current:
    movq %rdi, 32(%rdi)       /* ctx->rdi = live rdi value (before clobber) */
    pushq %rax
    movq %rax, 0(%rdi)
    popq %rax
    pushq %rcx
    movq %rcx, 8(%rdi)
    popq %rcx
    pushq %rdx
    movq %rdx, 16(%rdi)
    popq %rdx
    pushq %rsi
    movq %rsi, 24(%rdi)
    popq %rsi
    pushq %rbp
    movq %rbp, 80(%rdi)
    popq %rbp
    pushq %rbx
    movq %rbx, 72(%rdi)
    popq %rbx
    pushq %r8
    movq %r8, 40(%rdi)
    popq %r8
    pushq %r9
    movq %r9, 48(%rdi)
    popq %r9
    pushq %r10
    movq %r10, 56(%rdi)
    popq %r10
    pushq %r11
    movq %r11, 64(%rdi)
    popq %r11
    pushq %r12
    movq %r12, 88(%rdi)
    popq %r12
    pushq %r13
    movq %r13, 96(%rdi)
    popq %r13
    pushq %r14
    movq %r14,104(%rdi)
    popq %r14
    pushq %r15
    movq %r15,112(%rdi)
    popq %r15
    pushfq
    popq %rax
    movq %rax, 136(%rdi)      /* rflags */
    movq %rsp, %rax
    movq %rax, 120(%rdi)      /* rsp */
    call .Lss_ret              /* rsp[0] = .Lss_ret addr, rsp[8] = real ret addr */
.Lss_ret:
    movq 8(%rsp), %rax         /* real return address of the caller */
    movq %rax, 128(%rdi)       /* rip */
    movq (%rsp), %rbx          /* .Lss_ret address: drop it */
    addq $8, %rsp
    movq %rbx, %rax
    ret                         /* pops the real return address */
.size thread_save_current, .-thread_save_current
