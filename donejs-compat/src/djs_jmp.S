/* djs_jmp.S — setjmp / longjmp / context switch / coroutine entry for the
 * donejs JavaScript runtime port.
 *
 * djs_jmp_buf_t is eight uint64_t words: the callee-saved registers, the
 * stack pointer and the resume address. A saved context is restored by
 * loading those words and jumping to the resume address, which is the return
 * address of the most recent djs_setjmp / djs_context_switch / djs_coro_enter
 * call. Restoring the saved stack pointer and jumping to the saved return
 * address therefore behaves exactly like a normal return.
 *
 * The coroutine stack lives in a buffer allocated by djs_host_alloc; the
 * bootstrap entry runs the generator / async body on that stack and returns
 * control to the caller by context switch.
 */
    .section .text

    .global djs_setjmp
    .type   djs_setjmp, @function
djs_setjmp:
    mov     %rbx, 0(%rdi)
    mov     %rbp, 8(%rdi)
    mov     %r12, 16(%rdi)
    mov     %r13, 24(%rdi)
    mov     %r14, 32(%rdi)
    mov     %r15, 40(%rdi)
    lea     8(%rsp), %rax
    mov     %rax, 48(%rdi)
    mov     (%rsp), %rax
    mov     %rax, 56(%rdi)
    xor     %eax, %eax
    ret

    .global djs_longjmp
    .type   djs_longjmp, @function
djs_longjmp:
    test    %rdi, %rdi
    jz      .L_djs_root
    mov     48(%rdi), %rsp
    mov     0(%rdi), %rbx
    mov     8(%rdi), %rbp
    mov     16(%rdi), %r12
    mov     24(%rdi), %r13
    mov     32(%rdi), %r14
    mov     40(%rdi), %r15
    mov     %esi, %eax
    test    %esi, %esi
    jz      .L_djs_set_one
    jmp     *56(%rdi)
.L_djs_set_one:
    mov     $1, %eax
    jmp     *56(%rdi)
.L_djs_root:
    mov     djs_root_jmp(%rip), %rdi
    test    %rdi, %rdi
    jz      .L_djs_abort
    jmp     djs_longjmp
.L_djs_abort:
    ud2

    .global djs_context_switch
    .type   djs_context_switch, @function
djs_context_switch:
    mov     %rbx, 0(%rdi)
    mov     %rbp, 8(%rdi)
    mov     %r12, 16(%rdi)
    mov     %r13, 24(%rdi)
    mov     %r14, 32(%rdi)
    mov     %r15, 40(%rdi)
    lea     8(%rsp), %rax
    mov     %rax, 48(%rdi)
    mov     (%rsp), %rax
    mov     %rax, 56(%rdi)
    mov     0(%rsi), %rbx
    mov     8(%rsi), %rbp
    mov     16(%rsi), %r12
    mov     24(%rsi), %r13
    mov     32(%rsi), %r14
    mov     40(%rsi), %r15
    mov     48(%rsi), %rsp
    jmp     *56(%rsi)

    .global djs_coro_enter
    .type   djs_coro_enter, @function
djs_coro_enter:
    mov     %rbx, 0(%rdi)
    mov     %rbp, 8(%rdi)
    mov     %r12, 16(%rdi)
    mov     %r13, 24(%rdi)
    mov     %r14, 32(%rdi)
    mov     %r15, 40(%rdi)
    lea     8(%rsp), %rax
    mov     %rax, 48(%rdi)
    mov     (%rsp), %rax
    mov     %rax, 56(%rdi)
    mov     %rsp, %rax
    mov     %rsi, %rsp
    and     $-16, %rsp
    sub     $8, %rsp
    push    %rdi
    mov     %rcx, %rdi
    call    *%rdx
    pop     %rdi
    mov     56(%rdi), %r11
    mov     48(%rdi), %rsp
    mov     0(%rdi), %rbx
    mov     8(%rdi), %rbp
    mov     16(%rdi), %r12
    mov     24(%rdi), %r13
    mov     32(%rdi), %r14
    mov     40(%rdi), %r15
    movl    $1, %eax
    jmp     *%r11

    .section .data
    .global djs_root_jmp
    .type   djs_root_jmp, @object
    .size   djs_root_jmp, 8
djs_root_jmp:
    .quad   0

    .section .note.GNU-stack,"",@progbits
