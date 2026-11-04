/* linux/spinlock.h — spinlocks for Helix's single CPU.
 *
 * Helix runs on one CPU, but the timer interrupt preempts kernel threads. A
 * spinlock held by a thread that gets preempted would deadlock the next thread
 * that takes it. So on Helix a spin_lock also masks interrupts, and the saved
 * interrupt flag is restored on unlock. That is the irqsave form of Linux's API.
 * When Helix gains SMP, the acquire path gains a real spin on another CPU. */
#ifndef HELIX_COMPAT_LINUX_SPINLOCK_H
#define HELIX_COMPAT_LINUX_SPINLOCK_H

#include <linux/types.h>

#define COMPAT_RFLAGS_IF (1ul << 9)

typedef struct {
    volatile int locked;
} spinlock_t;

#define DEFINE_SPINLOCK(name) spinlock_t name = { 0 }
#define spin_lock_init(lock) do { (lock)->locked = 0; } while (0)

/* Mask interrupts and return the previous RFLAGS (for the restore below). */
static inline unsigned long compat_irq_save(void) {
    unsigned long flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static inline void compat_irq_restore(unsigned long flags) {
    if (flags & COMPAT_RFLAGS_IF) {
        __asm__ volatile("sti" ::: "memory");
    }
}

static inline void compat_spin_acquire(spinlock_t *lock) {
    while (__atomic_exchange_n(&lock->locked, 1, __ATOMIC_ACQUIRE)) {
        __asm__ volatile("pause");
    }
}

static inline void compat_spin_release(spinlock_t *lock) {
    __atomic_store_n(&lock->locked, 0, __ATOMIC_RELEASE);
}

/* The flags word is the caller's; the lock itself does not store it. */
#define spin_lock_irqsave(lock, flags) \
    do { (flags) = compat_irq_save(); compat_spin_acquire(lock); } while (0)

#define spin_unlock_irqrestore(lock, flags) \
    do { compat_spin_release(lock); compat_irq_restore(flags); } while (0)

#define spin_lock(lock) \
    do { (void)compat_irq_save(); compat_spin_acquire(lock); } while (0)

/* spin_lock has no saved flag to return, so unlock re-enables interrupts. This
 * is only correct when the caller was not already in an interrupt-off region;
 * drivers that nest should use the irqsave forms. */
#define spin_unlock(lock) \
    do { compat_spin_release(lock); __asm__ volatile("sti" ::: "memory"); } while (0)

#endif /* HELIX_COMPAT_LINUX_SPINLOCK_H */
