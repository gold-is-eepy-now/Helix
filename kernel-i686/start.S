/* start.S — Multiboot 1 header and 32-bit entry.
 *
 * Why Multiboot: Limine loads only x86-64 kernels (tested: "elf: Not an x86-64 ELF file" on
 * both the BIOS and the IA-32 UEFI loader). Multiboot is the standard boot protocol for
 * 32-bit x86 kernels; GRUB (BIOS) and QEMU's -kernel both implement it.
 *
 * The header must sit in the first 8 KiB of the image. The loader enters us with
 * EAX = 0x2BADB002 (Multiboot magic) and EBX = pointer to the Multiboot info block. */
    .section .multiboot, "a"
    .align 4
    .long 0x1BADB002                    /* magic */
    .long 0x00000003                    /* flags: align modules to 4 KiB, provide memory map */
    .long -(0x1BADB002 + 0x00000003)    /* checksum: magic + flags + checksum == 0 */

    .section .text
    .global _start
_start:
    cli
    movl $stack_top, %esp
    pushl %ebx                          /* arg 2: Multiboot info pointer */
    pushl %eax                          /* arg 1: Multiboot magic */
    call kmain
1:  cli
    hlt
    jmp 1b

    .section .bss
    .align 16
stack_bottom:
    .skip 16384
stack_top:

    .section .note.GNU-stack,"",@progbits
