/* gdt.h — Global Descriptor Table with ring-3 user segments. */

#ifndef HELIX_GDT_H
#define HELIX_GDT_H

#include <stddef.h>
#include <stdint.h>

/* GDT selectors (index << 3). RPL is added at use time. */
#define SEL_KERNEL_CODE 0x08
#define SEL_KERNEL_DATA 0x10
#define SEL_USER_CODE   0x18
#define SEL_USER_DATA   0x20

#define RPL0 0
#define RPL3 3

/* User selectors with ring-3 RPL. */
#define USER_CS (SEL_USER_CODE | RPL3)
#define USER_DS (SEL_USER_DATA | RPL3)

/* Maximum GDT entries we support. */
#define GDT_MAX_ENTRIES 16u

/* Build and load the GDT with user code/data segments appended.
 *
 * This adds user code/data segment selectors that can be used by user-space
 * code. The kernel's existing selectors remain valid; no far jump is needed.
 *
 * After calling this, user code can use USER_CS and USER_DATA to execute
 * at ring 3. The int $0x80 syscall gate (installed in idt.c) uses USER_CS
 * so that user code can invoke kernel services.
 */
void gdt_init(void);

/* Not used when TSS is not installed. */
void gdt_load_tss(uint64_t tss_phys, uint64_t hhdm_offset);

#endif /* HELIX_GDT_H */