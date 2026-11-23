/* linker.ld — i686 kernel. Loaded at 1 MiB, physical = virtual (no paging yet).
 * Two program headers: code is read+execute, data is read+write (so no RWX segment). */
ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);      /* R + X */
    data PT_LOAD FLAGS(6);      /* R + W */
}

SECTIONS
{
    . = 0x00100000;

    .text : {
        KEEP(*(.multiboot))     /* the loader needs this header near the start */
        *(.text .text.*)
    } :text

    .rodata : { *(.rodata .rodata.*) } :text

    .data : { *(.data .data.*) } :data
    .bss : {
        *(COMMON)
        *(.bss .bss.*)
    } :data

    /DISCARD/ : { *(.comment) *(.note*) }
}
