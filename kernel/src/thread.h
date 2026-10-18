/* thread.h — cooperative kernel threads and round-robin dispatch. */
#ifndef HELIX_THREAD_H
#define HELIX_THREAD_H

#include <stdbool.h>
#include <stdint.h>

#include "idt.h"

#define KERNEL_THREAD_MAX 8u
#define KERNEL_THREAD_STACK_PAGES 4u

typedef uint32_t thread_id_t;
typedef void (*thread_entry_t)(void *argument);

/* Initialize the bootstrap thread after the kernel has switched to its
 * protected stack. The scheduler is single-CPU and round-robin; interrupt
 * guards serialize shared state against PIT preemption but are not SMP locks.
 */
bool thread_system_init(uint64_t hhdm_offset);
thread_id_t thread_create(thread_entry_t entry, void *argument);
bool thread_yield(void);
__attribute__((noreturn)) void thread_exit(void);
registers_t *thread_schedule(registers_t *interrupted, bool timer_tick);
uint32_t thread_live_count(void);
uint64_t thread_switch_count(void);
uint64_t thread_preemption_count(void);

#endif /* HELIX_THREAD_H */
