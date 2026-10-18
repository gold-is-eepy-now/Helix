/* usermode_entry.S — controlled ring-3 entry and kernel return trampoline. */
.section .text
.global usermode_enter
.type usermode_enter, @function
usermode_enter:
    cli
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, usermode_saved_rsp(%rip)

    /* GS keeps the supervisor-only per-CPU syscall stack pointer in user
     * mode; user accesses to that page are blocked by the page tables. */
    movw $0x1B, %ax
    movw %ax, %gs

    pushq $0x1B                 /* user data selector, RPL 3 */
    pushq %rsi                  /* user RSP */
    pushq $0x2                   /* reserved bit only; keep IF clear */
    pushq $0x23                 /* user code selector, RPL 3 */
    pushq %rdi                  /* user RIP */
    iretq
.size usermode_enter, .-usermode_enter

.global usermode_kernel_return
.type usermode_kernel_return, @function
usermode_kernel_return:
    cli
    movq usermode_saved_rsp(%rip), %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbp
    popq %rbx
    ret
.size usermode_kernel_return, .-usermode_kernel_return

/* Fast syscall entry uses the same saved-frame layout as registers_t in
 * idt.h. SYSCALL leaves RSP unchanged, so switch to our guarded transition
 * stack through the supervisor-only GS data before calling C. */
.global usermode_syscall_entry
.type usermode_syscall_entry, @function
usermode_syscall_entry:
    movq %rsp, %gs:8
    movq %gs:0, %rsp
    pushq $0x1B                 /* user SS */
    pushq %gs:8                 /* user RSP */
    pushq %r11                  /* user RFLAGS */
    pushq $0x23                 /* user CS */
    pushq %rcx                  /* user RIP */
    pushq $0                    /* synthetic error code */
    pushq $128                  /* syscall vector */
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
    movq %rsp, %rdi
    call usermode_handle_syscall

    /* EXIT rewrites CS/RIP to the kernel return trampoline. */
    cmpq $0x08, 144(%rsp)
    je usermode_kernel_return

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
    addq $16, %rsp             /* vector and synthetic error code */
    movq 0(%rsp), %rcx         /* SYSRET RIP */
    movq 16(%rsp), %r11        /* SYSRET RFLAGS */
    andq $~0x700, %r11         /* keep user IF/TF/DF clear for this ABI */
    movq 24(%rsp), %rsp        /* restore user stack */
    sysretq
.size usermode_syscall_entry, .-usermode_syscall_entry

.section .note.GNU-stack,"",@progbits
