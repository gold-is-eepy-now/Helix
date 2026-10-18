/* kernel_stack.h — guarded, non-executable kernel stack. */
#ifndef HELIX_KERNEL_STACK_H
#define HELIX_KERNEL_STACK_H

#include <stdbool.h>
#include <stdint.h>

#define KERNEL_STACK_PAGE_COUNT 8u
#define KERNEL_STACK_GUARD_VIRT 0xffff900000200000ULL
#define KERNEL_STACK_BASE (KERNEL_STACK_GUARD_VIRT + 4096u)
#define KERNEL_STACK_TOP (KERNEL_STACK_BASE + KERNEL_STACK_PAGE_COUNT * 4096u)
#define KERNEL_STACK_NX_PROBE_VIRT 0xffff900000300000ULL

typedef void (*kernel_entry_fn)(void);

bool kernel_stack_init(uint64_t hhdm_offset);
bool kernel_stack_active(void);
bool kernel_stack_probe_guard(uint64_t *error_out);
bool kernel_stack_probe_nx(uint64_t *error_out);

/* Switch to the mapped stack and invoke entry. Does not return. */
__attribute__((noreturn)) void kernel_stack_enter(uintptr_t top,
                                                  kernel_entry_fn entry);

/* Assembly probes and recovery targets used by the protection self-test. */
int kernel_stack_guard_attempt(uintptr_t address);
void kernel_stack_guard_recovery(void);
int kernel_stack_exec_attempt(uintptr_t target);
void kernel_stack_exec_recovery(void);

#endif /* HELIX_KERNEL_STACK_H */
