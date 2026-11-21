/* LOOM kernel link layout (x86_64, Limine boot protocol).
 * Same shape as Helix's kernel/linker.ld: the kernel lives in the top 2 GiB, the Limine
 * requests sit in their own segment where the bootloader looks for them, and each kind of
 * memory gets its own page-aligned segment with the right permissions. */
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(kmain)

PHDRS
{
    limine_requests PT_LOAD FLAGS(0x06);
    text            PT_LOAD FLAGS(0x05);
    rodata          PT_LOAD FLAGS(0x04);
    data            PT_LOAD FLAGS(0x06);
}

SECTIONS
{
    . = 0xffffffff80000000;

    .limine_requests : {
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
    } :limine_requests

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .text : { *(.text .text.*) } :text

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .rodata : { *(.rodata .rodata.*) } :rodata

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .data : {
        *(.data .data.*)
        *(.bss .bss.*)
        *(COMMON)
    } :data

    /DISCARD/ : { *(.eh_frame*) *(.note .note.*) }
}
