/* Test Cells: machine code that runs in ring 3 in its own address space. Each one is copied into a
 * fresh user page at 0x400000 before it runs, so its absolute addresses are the ones it sees. */
    .text

    .globl cell_exit_start
cell_exit_start:
    movl $1, %eax            /* SYS_EXIT */
    movl $7, %edi            /* status 7 */
    int $0x80
    jmp cell_exit_start      /* never reached */
    .globl cell_exit_end
cell_exit_end:

    .globl cell_fault_start
cell_fault_start:
    movabsq $0x600000, %rax  /* an address this Cell does not map */
    movq (%rax), %rbx        /* page fault: the kernel must kill the Cell */
    jmp cell_fault_start
    .globl cell_fault_end
cell_fault_end:


/* Cell A: sends a typed message on handle 0 (SEND), then tries three things it may not do, records
 * the results at 0x500080, and reads Cell B's secret page, which it does not map. The fault kills it. */
    .globl cell_a_start
cell_a_start:
    movabsq $0x500080, %rbx
    movl $2, %eax            /* SYS_SEND */
    movl $0, %edi            /* handle 0: SEND on the channel */
    movl $1, %esi            /* the channel's declared type */
    movabsq $0x500000, %rdx  /* payload */
    int $0x80
    movq %rax, 0(%rbx)       /* expect 0 */
    movl $2, %eax
    movl $0, %edi
    movl $2, %esi            /* wrong type */
    movabsq $0x500000, %rdx
    int $0x80
    movq %rax, 8(%rbx)       /* expect -22 (EINVAL) */
    movl $2, %eax
    movl $1, %edi            /* handle 1: RECV only */
    movl $1, %esi
    movabsq $0x500000, %rdx
    int $0x80
    movq %rax, 16(%rbx)      /* expect -1 (EPERM) */
    movabsq $0x600000, %rax  /* Cell B's secret: not mapped here */
    movq (%rax), %rcx        /* page fault: this Cell is killed */
    jmp cell_a_start
    .globl cell_a_end
cell_a_end:

/* Cell B: receives on handle 0 into 0x500040, logs the payload, tries to SEND on handle 1 (not held),
 * records the results at 0x500080, and exits 0. */
    .globl cell_b_start
cell_b_start:
    movabsq $0x500080, %rbx
    movl $3, %eax            /* SYS_RECV */
    movl $0, %edi
    movabsq $0x500040, %rsi  /* receives type (4 bytes) then payload (32) */
    int $0x80
    movq %rax, 24(%rbx)      /* expect 0 */
    movl $4, %eax            /* SYS_LOG */
    movabsq $0x500044, %rdi
    movl $12, %esi           /* "hello from A" */
    int $0x80
    movl $2, %eax            /* SYS_SEND on a handle this Cell does not hold */
    movl $1, %edi
    movl $1, %esi
    movabsq $0x500040, %rdx
    int $0x80
    movq %rax, 32(%rbx)      /* expect -9 (EBADF) */
    movl $1, %eax            /* SYS_EXIT */
    xorl %edi, %edi
    int $0x80
    jmp cell_b_start
    .globl cell_b_end
cell_b_end:

    .section .note.GNU-stack,"",@progbits
