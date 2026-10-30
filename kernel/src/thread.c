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

#include <stddef.h>
#include <stdint.h>

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
    t->ctx.rsp = vaddr + THREAD_STACK_BYTES - 16u;
    t->ctx.rflags = THREAD_INITIAL_RFLAGS;
    return t;
}

void thread_yield(void) {
    thread_t *cur = g_current;
    if (cur == NULL) return;
    thread_t *next = rr_next(g_run_queue, cur);
    if (next == cur) return;
    cur->state = THREAD_READY;
    next->state = THREAD_RUNNING;
    g_current = next; /* the switch never returns here; record who now runs */
    /* Saves cur's callee-saved registers and this call's resume rip/rsp/
     * rflags into cur->ctx, then loads next's context and jumps into it. */
    thread_switch_to(&cur->ctx, &next->ctx);
}

void thread_tick(void) {
    thread_t *cur = thread_current();
    if (cur == NULL || cur->state != THREAD_RUNNING) return;
    cur->ticks++;
    if (cur->ticks < THREAD_QUANTUM_TICKS) return;

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

    thread_t *next = rr_next(g_run_queue, cur);
    if (next == cur) return;
    cur->state = THREAD_READY;
    next->state = THREAD_RUNNING;
    g_current = next;
    /* Load-only: the ISR saved cur's context above, so just resume next. */
    thread_switch_load(&next->ctx);
}

void thread_block_current(void) {
    thread_t *cur = thread_current();
    if (cur != NULL) cur->state = THREAD_WAITING;
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
    kfree(cur);
    g_current = g_run_queue;
}

thread_t *thread_current(void) {
    return g_current;
}

void thread_demo_main(void) {
    for (;;) {
        thread_yield();
    }
}
