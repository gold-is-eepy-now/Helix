/* gdt.S — load Helix's GDT, reload CS/data selectors, and activate the TSS. */
.section .text
.global gdt_load
.type gdt_load, @function
gdt_load:
    lgdt (%rdi)
    leaq 1f(%rip), %rax
    pushq $0x08
    pushq %rax
    lretq
1:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    xorw %ax, %ax
    movw %ax, %fs
    movw %ax, %gs
    movw $0x28, %ax
    ltr %ax
    ret
.size gdt_load, .-gdt_load
