/* User Cell images, embedded in the kernel as read-only data. Each is a flat binary built from
 * user/ by the Makefile and linked at 0x400000. The kernel copies these bytes into a Cell's pages. */
    .section .rodata
    .globl hello_image_start
    .globl hello_image_end
    .align 16
hello_image_start:
    .incbin "build/user/hello.bin"
hello_image_end:

    .section .note.GNU-stack,"",@progbits
