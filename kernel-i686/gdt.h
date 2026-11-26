/* gdt.h — the kernel's own Global Descriptor Table for i686. */
#ifndef HELIX_I686_GDT_H
#define HELIX_I686_GDT_H

#define GDT_KERNEL_CODE 0x08    /* selector: ring-0 flat code segment */
#define GDT_KERNEL_DATA 0x10    /* selector: ring-0 flat data segment */

/* Install our own GDT and reload the segment registers. Must run before interrupts are
 * enabled: the IDT's gates name GDT_KERNEL_CODE. */
void gdt_init(void);

#endif /* HELIX_I686_GDT_H */
