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

/* Implemented in thread_switch.S. Save the full context of the calling
 * thread into `out_ctx` and return normally; the caller resumes right after
 * the call. Used by blocking IPC to park a thread's state before sleeping. */
void thread_save_current(thread_context_t *out_ctx);

/* Implemented in thread_switch.S. Save the caller (out_ctx) and load the
 * incoming context (in_ctx): the run-queue hop for `thread_yield`. */
void thread_switch_to(thread_context_t *out_ctx, thread_context_t *in_ctx);

/* Implemented in thread_switch.S. Load `in_ctx` and jump into it. The
 * caller must already have stored its own state (thread_yield via
 * thread_switch_to, timer preemption via the interrupt frame rebuild in
 * thread_tick). */
void thread_switch_load(thread_context_t *in_ctx);

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

/* Mark the current thread WAITING (used by blocking IPC). */
void thread_block_current(void);

/* Mark an arbitrary thread's state (READY / RUNNING / WAITING / DONE). */
void thread_set_state(thread_t *thread, thread_state_t state);

/* Hand the CPU to a thread whose context was parked via thread_save_current
 * (blocking IPC deliver). The caller marks the target thread READY first; the
 * jump lands at the parked thread's saved rip and never returns. */
void thread_resume_context(thread_context_t *ctx, thread_t *thread);

/* Mark the current thread DONE and release it. */
void thread_exit_current(void);

/* The thread that is running right now (NULL before thread_init). */
thread_t *thread_current(void);

/* Sender demo: parks or delivers on the default rendezvous channel. */
void thread_sender_main(void);

/* Receiver demo: blocks until the sender delivers a value. */
void thread_receiver_main(void);

#endif /* HELIX_THREAD_H */
