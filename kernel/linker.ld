/* linker.ld — Helix x86_64 kernel link layout.
 *
 * Based on the Limine C template linker script. Three jobs:
 *   1. Force a 64-bit ELF output whose entry point is kmain.
 *   2. Place the kernel in the top 2 GiB of the address space, which the
 *      Limine protocol requires and which lets the compiler use the small
 *      "kernel" code model.
 *   3. Give each kind of memory its own page-aligned segment with correct
 *      permissions (execute for code, read-only for constants, writable for
 *      data), and collect the Limine requests into their own segment.
 */
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(kmain)

PHDRS
{
    limine_requests PT_LOAD FLAGS(0x06); /* read + write */
    text            PT_LOAD FLAGS(0x05); /* read + execute */
    rodata          PT_LOAD FLAGS(0x04); /* read */
    data            PT_LOAD FLAGS(0x06); /* read + write */
}

SECTIONS
{
    /* Topmost 2 GiB: required by Limine and ideal for -mcmodel=kernel. */
    . = 0xffffffff80000000;

    /* Limine scans this segment for protocol requests. */
    .limine_requests : {
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
    } :limine_requests

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .text : {
        *(.text .text.*)
    } :text

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .rodata : {
        *(.rodata .rodata.*)
    } :rodata

    .note.gnu.build-id : {
        *(.note.gnu.build-id)
    } :rodata

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .data : {
        *(.data .data.*)
    } :data

    /* .bss must be the last thing in the writable segment so the file on
     * disk does not carry megabytes of zeroes. */
    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } :data

    /* Nothing in the kernel needs these, and some hosts choke on them. */
    /DISCARD/ : {
        *(.eh_frame*)
        *(.note .note.*)
        *(.got .got.*)
    }
}
