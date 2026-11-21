/* Cell image layout: flat binary at 0x400000, the address the kernel maps the image to.
 * Cells have no writable globals: all state lives on the stack. A flat image cannot carry
 * .bss, so the linker script puts nothing there. */
ENTRY(_start)
SECTIONS
{
    . = 0x400000;
    .text : {
        KEEP(*(.text.start))
        *(.text .text.*)
    }
    .rodata : { *(.rodata .rodata.*) }
    .data : { *(.data .data.*) }
    /DISCARD/ : { *(.bss .bss.*) *(.eh_frame*) *(.note*) }
}
