/* Cell entry point: the kernel jumps here with an empty stack page. Call main, then exit with its
 * return value. Placed first in .text so the image starts at the entry address. */
    .section .text.start, "ax"
    .globl _start
_start:
    xorl %ebp, %ebp
    call main
    movq %rax, %rdi          /* exit status */
    movl $1, %eax            /* SYS_EXIT */
    int $0x80
1:  jmp 1b

    .section .note.GNU-stack,"",@progbits
