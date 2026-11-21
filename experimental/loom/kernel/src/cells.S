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

    .section .note.GNU-stack,"",@progbits
