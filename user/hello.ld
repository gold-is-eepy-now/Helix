ENTRY(_start)
PHDRS
{
    text PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
}
SECTIONS
{
    . = 0x400000;
    .text : { *(.text*) } :text
    . = ALIGN(0x1000);
    .rodata : { *(.rodata*) } :rodata
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
