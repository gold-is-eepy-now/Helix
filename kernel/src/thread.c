/* thread.c — kernel threads and a simple round-robin scheduler.
 *
 * The running thread owns a `thread_context_t`: every general-purpose
 * register plus rsp, rip and rflags. `thread_yield` saves the callee-saved
 * registers and the resume point of its caller into the current context,
 * then switches; the timer preemption path (`thread_tick`) rebuilds the
 * interrupted thread's whole context from the interrupt frame and switches
 * with the load-only entry point, since the ISR already owns the save side.
 *
 * Switching back to a thread therefore resumes it exactly where it left off,
 * with interrupts restored — whether it left off inside `thread_yield` or in
 * the middle of some other function when the timer preempted it.
 */
#include "thread.h"
#include "klog.h"
#include "usermode.h"
#include "gdt.h"

#include <stddef.h>
#include <stdint.h>

#include "channel.h"
#include "heap.h"
#include "idt.h"
#include "kernel_stack.h"
#include "pmm.h"
#include "panic.h"
#include "vmm.h"

#define THREAD_STACK_BYTES (2u * 4096u)
#define THREAD_QUANTUM_TICKS 2u


/* RFLAGS value for a freshly created thread: interrupts enabled (bit 1 is
 * the architecturally fixed "always 1"). */
#define THREAD_INITIAL_RFLAGS 0x202ULL
#define RFLAGS_IF (1ULL << 9)

/* Switches must not be preempted. The timer ISR runs thread_tick, which reads
 * g_current as "the thread being interrupted". thread_yield and friends move
 * g_current *before* the register state is saved, so a timer tick in that gap
 * would save the outgoing thread's state into the incoming thread's context
 * (and leave the outgoing one with a stale rip). The result is a thread that
 * later resumes at address 0. Clearing IF for the duration of a switch closes
 * the window; the caller restores its own IF afterwards. */
static uint64_t irq_save_disable(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static void irq_restore(uint64_t flags) {
    if (flags & RFLAGS_IF) {
        __asm__ volatile("sti" : : : "memory");
    }
}

struct thread {
    uint64_t id;
    thread_state_t state;
    thread_context_t ctx;
    struct thread *next;
    uint64_t ticks;
};

/* Implemented in thread_switch.S. */
void thread_switch_to(thread_context_t *out_ctx, thread_context_t *in_ctx);
void thread_switch_load(thread_context_t *in_ctx);
void thread_save_current(thread_context_t *out_ctx);

static thread_t *g_run_queue;
static thread_t *g_current;
static uint64_t g_next_thread_id;
static uintptr_t g_bump_vaddr;

static thread_t *rr_next(thread_t *head, thread_t *cur) {
    thread_t *t = head;
    while (t != NULL && t != cur) {
        t = t->next;
    }
    if (t == NULL) return head;
    thread_t *after = t->next;
    return (after != NULL) ? after : head;
}

void thread_init(void) {
    g_next_thread_id = 1;
    g_bump_vaddr = (uintptr_t)KERNEL_STACK_TOP;

    thread_t *boot = kzalloc(sizeof(thread_t));
    if (boot == NULL) return;
    boot->id = 0;
    boot->state = THREAD_RUNNING;
    boot->ticks = 0;
    boot->next = NULL;
    g_run_queue = boot;
    g_current = boot;
    /* Only rip/rsp/rflags matter until the boot thread first yields, and
     * thread_switch_to overwrites all three then. */
    boot->ctx.rflags = THREAD_INITIAL_RFLAGS;

    /* M8: the demo pair shares one rendezvous channel. The receiver is
     * queued first and parks; the sender is queued ahead of it, so the first
     * context switch lands on the sender, which delivers to the parked
     * receiver. */
    channel_init();
    thread_create(thread_receiver_main);
    thread_create(thread_sender_main);
}

thread_t *thread_create(void (*entry)(void)) {
    thread_t *t = kzalloc(sizeof(thread_t));
    if (t == NULL) return NULL;
    t->id = g_next_thread_id++;
    t->state = THREAD_READY;
    t->ticks = 0;
    t->next = g_run_queue;
    g_run_queue = t;

    uintptr_t vaddr = (uintptr_t)g_bump_vaddr;
    for (uintptr_t a = vaddr; a < (uintptr_t)(vaddr + THREAD_STACK_BYTES); a += PAGE_SIZE) {
        uint64_t f = pmm_alloc_frame();
        if (f == 0) {
            g_run_queue = t->next; /* unlink; thread never ran */
            return NULL;
        }
        if (!vmm_map((uint64_t)a, f, VMM_PRESENT | VMM_WRITE)) {
            pmm_free_frame(f);
            g_run_queue = t->next; /* unlink; thread never ran */
            return NULL;
        }
    }
    g_bump_vaddr += THREAD_STACK_BYTES;
    /* A new thread has no saved state; its resume point is simply `entry`
     * running on the fresh stack (16-byte aligned, per the SysV ABI). */
    t->ctx.rip = (uint64_t)(uintptr_t)entry;
    /* The first resume `ret`s into entry with rsp = this value. The SysV ABI wants
     * rsp == 8 (mod 16) at a function's first instruction, so leave 8 bytes, not 16. */
    t->ctx.rsp = vaddr + THREAD_STACK_BYTES - 8u;
    t->ctx.rflags = THREAD_INITIAL_RFLAGS;
    t->ctx.cs = 0x08;  /* kernel code selector */
    t->ctx.ss = 0x10;  /* kernel data selector */
    klog_event(KLOG_SCHED, "thread %llu created", (unsigned long long)t->id);
    return t;
}

void thread_yield(void) {
    thread_t *cur = g_current;
    if (cur == NULL) return;
    uint64_t irq = irq_save_disable();
    thread_t *next = rr_next(g_run_queue, cur);
    if (next == cur) {
        irq_restore(irq);
        return;
    }
    cur->state = THREAD_READY;
    next->state = THREAD_RUNNING;
    g_current = next; /* the switch never returns here; record who now runs */
    /* Saves cur's callee-saved registers and this call's resume rip/rsp/
     * rflags into cur->ctx, then loads next's context and jumps into it.
     * When this thread is scheduled again, control returns here and the
     * restore below runs with this thread's own saved IF bit. */
    thread_switch_to(&cur->ctx, &next->ctx);
    irq_restore(irq);
}

void thread_tick(void) {
    thread_t *cur = thread_current();
    if (cur == NULL || cur->state != THREAD_RUNNING) return;
    cur->ticks++;
    if (cur->ticks < THREAD_QUANTUM_TICKS) return;
    /* Two cases must not switch threads: the interrupt came from ring 3 (the
     * user code has no kernel context to save), or a process is active (its
     * address space and kernel stack are global state; switching away and
     * back in the middle of a process corrupts them). Keep the quantum, retry
     * on the next tick. */
    if (g_interrupt_frame[18] != SEL_KERNEL_CODE || usermode_is_active()) return;

    /* The timer ISR interrupted `cur`; rebuild its context from the frame so
     * the switch resumes it exactly where it was interrupted. */
    const uint64_t *frame = g_interrupt_frame;
    thread_context_t *ctx = &cur->ctx;
    ctx->r15 = frame[0];
    ctx->r14 = frame[1];
    ctx->r13 = frame[2];
    ctx->r12 = frame[3];
    ctx->r11 = frame[4];
    ctx->r10 = frame[5];
    ctx->r9 = frame[6];
    ctx->r8 = frame[7];
    ctx->rbp = frame[8];
    ctx->rdi = frame[9];
    ctx->rsi = frame[10];
    ctx->rdx = frame[11];
    ctx->rcx = frame[12];
    ctx->rbx = frame[13];
    ctx->rax = frame[14];
    /* frame layout: r15@0 … rax@14, vector@15, err@16, rip@17, cs@18,
     * rflags@19, interrupted rsp@20. cs/ss stay constant in kernel mode, so
     * rip/rflags/rsp are all a resume needs. */
    ctx->rip = frame[17];
    ctx->rflags = frame[19];
    ctx->rsp = frame[20];
    ctx->cs = frame[18];  /* saved CS from interrupt frame */
    ctx->ss = frame[18];  /* SS stays same as CS in kernel mode for now */

    thread_t *next = rr_next(g_run_queue, cur);
    if (next == cur) return;
    klog_event(KLOG_SCHED, "preempt %llu -> %llu", (unsigned long long)cur->id,
               (unsigned long long)next->id);
    cur->state = THREAD_READY;
    next->state = THREAD_RUNNING;
    g_current = next;
    /* Load-only: the ISR saved cur's context above, so just resume next. */
    thread_switch_load(&next->ctx);
}

void thread_block_current(void) {
    thread_t *cur = thread_current();
    if (cur == NULL) return;
    /* A delivery marks the parked thread READY and resumes it through the
     * saved context; the load-only switch lands back here. Such a thread must
     * fall through instead of parking again. */
    if (cur->state == THREAD_READY || cur->state == THREAD_DONE) return;
    uint64_t irq = irq_save_disable();
    thread_set_state(cur, THREAD_WAITING);
    thread_t *next = rr_next(g_run_queue, cur);
    if (next == cur) {
        irq_restore(irq);
        return;
    }
    klog_event(KLOG_SCHED, "block %llu -> %llu", (unsigned long long)cur->id,
               (unsigned long long)next->id);
    g_current = next;
    /* Save this thread's full state, then run next. The thread resumes here later
     * (thread_resume_context), so its saved context must be the real one. */
    thread_switch_to(&cur->ctx, &next->ctx);
    irq_restore(irq);
}

void thread_set_state(thread_t *thread, thread_state_t state) {
    if (thread != NULL) thread->state = state;
}

void thread_resume_context(thread_context_t *ctx, thread_t *thread) {
    irq_save_disable(); /* the resumed context restores its own IF bit */
    klog_event(KLOG_SCHED, "resume %llu", thread ? (unsigned long long)thread->id : 0ull);
    g_current = thread;
    thread_switch_load(ctx);
}

void thread_exit_current(void) {
    thread_t *cur = thread_current();
    if (cur == NULL) return;
    thread_t **pp = &g_run_queue;
    while (*pp != NULL && *pp != cur) {
        pp = &(*pp)->next;
    }
    if (*pp != NULL) {
        *pp = cur->next;
        cur->next = NULL;
    }
    klog_event(KLOG_SCHED, "thread %llu exited", (unsigned long long)cur->id);
    kfree(cur);
    g_current = g_run_queue;
}

void thread_sender_main(void) {
    channel_send(channel_default(), 0x2a);
    for (;;) {
        thread_yield();
    }
}

void thread_receiver_main(void) {
    uint64_t value = 0;
    bool ok = channel_recv(channel_default(), &value);
    kprintf("[thread] receiver got value=%llu ok=%d\n",
            (unsigned long long)value, ok ? 1 : 0);
    for (;;) {
        thread_yield();
    }
}

thread_t *thread_current(void) {
    return g_current;
}

void thread_demo_main(void) {
    for (;;) {
        thread_yield();
    }
}
