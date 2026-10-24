/* sync.h — single-CPU preemption guards.
 *
 * Until Helix has SMP support, disabling maskable interrupts serializes short
 * kernel critical sections against PIT-driven thread preemption. This is not
 * an SMP lock and guarded sections must never block or explicitly yield.
 */
#ifndef HELIX_SYNC_H
#define HELIX_SYNC_H

#include <stdint.h>

#include "io.h"

typedef struct {
    uint64_t flags;
} irq_guard_t;

static inline irq_guard_t irq_guard_enter(void) {
    irq_guard_t guard = { .flags = read_rflags() };
    cpu_disable_interrupts();
    __asm__ volatile("" : : : "memory");
    return guard;
}

static inline void irq_guard_leave(irq_guard_t *guard) {
    __asm__ volatile("" : : : "memory");
    if (guard->flags & (1u << 9)) cpu_enable_interrupts();
}

#define IRQ_GUARD \
    irq_guard_t _irq_guard __attribute__((cleanup(irq_guard_leave))) = \
        irq_guard_enter()

#endif /* HELIX_SYNC_H */
