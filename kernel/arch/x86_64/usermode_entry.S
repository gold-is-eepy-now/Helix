/* usermode.S — enter ring 3 and return to the kernel context that entered it.
 *
 * usermode_run saves the kernel's callee-saved registers and stack pointer,
 * points the TSS at the current kernel stack (so ring-3 traps land there), and
 * uses iretq to drop to ring 3. usermode_return restores that saved context and
 * returns the status from usermode_run, abandoning the user frame.
 */
    .intel_syntax noprefix
    .text
    .code64

    .global usermode_run
    .global usermode_return

/* uint64_t usermode_run(uint64_t entry, uint64_t user_rsp, uint64_t *save,
 *                        uint64_t user_cs, uint64_t user_ss)
 *   rdi = entry point, rsi = user stack pointer, rdx = 8-qword save area,
 *   rcx = ring-3 code selector, r8 = ring-3 data selector. */
usermode_run:
    mov [rdx + 0], rsp
    mov [rdx + 8], rbx
    mov [rdx + 16], rbp
    mov [rdx + 24], r12
    mov [rdx + 32], r13
    mov [rdx + 40], r14
    mov [rdx + 48], r15

    /* Ring-3 interrupts and int $0x80 must land below this stack frame. */
    push rdi
    push rsi
    push rcx
    push r8
    lea rdi, [rsp + 32]
    call gdt_set_rsp0
    pop r8
    pop rcx
    pop rsi
    pop rdi

    /* Ring-3 data segments: DS and ES use the user data selector. */
    mov ax, r8w
    mov ds, ax
    mov es, ax

    /* iretq frame, pushed last first: SS, RSP, RFLAGS, CS, RIP. Interrupts stay
     * masked in ring 3 for now; the user program does not need them. */
    push r8
    push rsi
    push 0x002
    push rcx
    push rdi
    iretq

/* void usermode_return(uint64_t *save, uint64_t status) */
usermode_return:
    mov rax, rsi
    mov rsp, [rdi + 0]
    mov rbx, [rdi + 8]
    mov rbp, [rdi + 16]
    mov r12, [rdi + 24]
    mov r13, [rdi + 32]
    mov r14, [rdi + 40]
    mov r15, [rdi + 48]
    mov cx, 0x10
    mov ds, cx
    mov es, cx
    sti
    ret
