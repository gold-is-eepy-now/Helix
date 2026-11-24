/* Stack transition and controlled execute-fault recovery helper. */
.section .text
.global kernel_stack_enter
.type kernel_stack_enter, @function
kernel_stack_enter:
    cli
    movq %rdi, %rsp
    andq $-16, %rsp
    xorq %rbp, %rbp
    call *%rsi
1:
    cli
    hlt
    jmp 1b
.size kernel_stack_enter, .-kernel_stack_enter

/* Touch the unmapped guard page. The expected #PF redirects execution to
 * kernel_stack_guard_recovery, which returns through this helper's caller. */
.global kernel_stack_guard_attempt
.type kernel_stack_guard_attempt, @function
kernel_stack_guard_attempt:
    subq $8, %rsp
    movq (%rdi), %rax
    addq $8, %rsp
    xorl %eax, %eax
    ret
.size kernel_stack_guard_attempt, .-kernel_stack_guard_attempt

.global kernel_stack_guard_recovery
.type kernel_stack_guard_recovery, @function
kernel_stack_guard_recovery:
    addq $8, %rsp
    movl $1, %eax
    ret
.size kernel_stack_guard_recovery, .-kernel_stack_guard_recovery

/* Invoke target with proper SysV stack alignment. A #PF at the NX page is
 * redirected to kernel_stack_exec_recovery, which discards both this call's
 * target return slot and the alignment slot before returning to the caller. */
.global kernel_stack_exec_attempt
.type kernel_stack_exec_attempt, @function
kernel_stack_exec_attempt:
    subq $8, %rsp
    call *%rdi
    addq $8, %rsp
    xorl %eax, %eax
    ret
.size kernel_stack_exec_attempt, .-kernel_stack_exec_attempt

.global kernel_stack_exec_recovery
.type kernel_stack_exec_recovery, @function
kernel_stack_exec_recovery:
    addq $16, %rsp
    movl $1, %eax
    ret
.size kernel_stack_exec_recovery, .-kernel_stack_exec_recovery
