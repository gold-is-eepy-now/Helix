/* LOOM kernel-side declarations. Freestanding C11: no host libc. */
#ifndef LOOM_KERNEL_H
#define LOOM_KERNEL_H

#include <stdint.h>
#include <stdbool.h>

/* Serial (serial.c) */
void serial_init(void);
void serial_putc(char c);
void serial_puts(const char *s);
void serial_putu64(uint64_t v);
void serial_puthex64(uint64_t v);

/* Boot (boot.c) */
__attribute__((noreturn)) void kpanic(const char *msg);
extern uint64_t g_hhdm_offset;
static inline void *phys_to_virt(uint64_t phys) { return (void *)(phys + g_hhdm_offset); }

/* Physical frames (pmm.c) */
struct limine_memmap_response;
void pmm_init(const struct limine_memmap_response *mm);
uint64_t pmm_alloc_frame(void);            /* 0 when none is free */
void pmm_free_frame(uint64_t phys);
uint64_t pmm_free_count(void);

/* Address spaces (vm.c) */
#define PTE_USER_RW  (2ull | 4ull)        /* writable, user */
#define PTE_USER_RX  (0ull)               /* user, read-only, executable (no NX bit) */
uint64_t vm_current_cr3(void);
uint64_t vm_new_space(void);
bool     vm_map_user(uint64_t pml4, uint64_t virt, uint64_t phys, uint64_t flags);
uint64_t vm_translate(uint64_t pml4, uint64_t virt);
uint64_t vm_unmap_user(uint64_t pml4, uint64_t virt);
void     vm_switch(uint64_t pml4);
void     vm_free_space(uint64_t pml4);

/* Self-tests and the scenario */
bool mm_selftest(void);
bool cell_selftest(void);
bool chan_selftest(void);
bool loom_demo_run(void);

#endif
