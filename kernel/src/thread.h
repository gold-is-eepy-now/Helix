/* thread.h — kernel threads and a round-robin scheduler. */
#ifndef HELIX_THREAD_H
#define HELIX_THREAD_H

#include <stdbool.h>
#include <stdint.h>

/* Scheduler state for one kernel thread. A context switch restores every
 * general-purpose register, then RFLAGS, then resumes at `rip` with `rsp` as
 * the thread's stack pointer — the same state `iretq` would restore, which
 * is what makes it safe to switch away from a thread preempted inside an
 * interrupt handler. RSP/RIP/RFLAGS are separate fields because a plain `ret`
 * can only resume at an address that happens to sit on the stack; that trick
 * broke whenever the saved rsp was not a return-address slot. */
typedef struct {
    uint64_t rax, rcx, rdx, rsi, rdi;
    uint64_t r8, r9, r10, r11, rbx, rbp;
    uint64_t r12, r13, r14, r15;
    uint64_t rsp;     /* offset 120 */
    uint64_t rip;     /* offset 128 */
    uint64_t rflags;  /* offset 136 */
} thread_context_t;

typedef enum {
    THREAD_READY = 0,
    THREAD_RUNNING,
    THREAD_WAITING,
    THREAD_DONE,
} thread_state_t;

struct thread;

typedef struct thread thread_t;

/* Create a new kernel thread running `entry`. Returns NULL on allocation
 * failure; the caller checks the object manager afterwards. */
thread_t *thread_create(void (*entry)(void));

/* Mark the current thread (the boot thread on entry) as RUNNING. */
void thread_init(void);

/* Yield the CPU to the next thread in the round-robin queue. Saves the
 * callee-saved registers plus the resume rip/rsp/rflags of the caller, so
 * returning from `thread_yield` happens whenever the scheduler switches back
 * to this thread. */
void thread_yield(void);

/* Timer tick (IRQ0): preempt the running thread once its quantum expires.
 * The interrupted thread's full context comes from the interrupt frame. */
void thread_tick(void);

/* Mark the current thread WAITING (used by blocking IPC later). */
void thread_block_current(void);

/* Mark the current thread DONE and release it. */
void thread_exit_current(void);

/* The thread that is running right now (NULL before thread_init). */
thread_t *thread_current(void);

/* Demo body for the first extra thread: yields forever. */
void thread_demo_main(void);

#endif /* HELIX_THREAD_H */
