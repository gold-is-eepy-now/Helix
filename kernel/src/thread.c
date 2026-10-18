/* thread.c — single-CPU kernel scheduler.
 *
 * Threads share the kernel address space and each receive a guarded, NX
 * stack. Scheduling occurs at explicit yield/exit points and on PIT timer
 * interrupts. Scheduler mutation currently assumes one CPU; shared kernel
 * services still need synchronization before concurrent use.
 */
#include "thread.h"

#include <stddef.h>

#include "kstring.h"
#include "io.h"
#include "panic.h"
#include "pit.h"
#include "pmm.h"
#include "sync.h"
#include "vmm.h"

#define THREAD_STACK_ARENA 0xFFFF900001000000ULL
#define THREAD_STACK_STRIDE (KERNEL_THREAD_STACK_PAGES + 1u)

typedef enum {
    THREAD_FREE = 0,
    THREAD_BUILDING,
    THREAD_READY,
    THREAD_RUNNING,
    THREAD_BLOCKED,
    THREAD_DEAD,
} thread_state_t;

typedef struct {
    thread_id_t id;
    thread_state_t state;
    uint64_t rsp;
    uint64_t stack_base;
    uintptr_t stack_phys;
    uint64_t wake_tick;
    bool timed_block;
    thread_entry_t entry;
    void *argument;
} kernel_thread_t;

static kernel_thread_t g_threads[KERNEL_THREAD_MAX];
static uint32_t g_current;
static thread_id_t g_next_id;
static uint64_t g_switches;
static uint64_t g_preemptions;
static uint64_t g_hhdm;
static bool g_ready;

static __attribute__((noreturn)) void thread_bootstrap(void) {
    kernel_thread_t *thread = &g_threads[g_current];
    thread_entry_t entry = thread->entry;
    void *argument = thread->argument;
    entry(argument);
    thread_exit();
}

static bool stack_region_clear(uint64_t base) {
    for (uint64_t page = 0; page < THREAD_STACK_STRIDE; page++) {
        if (vmm_get_mapping(base + page * PAGE_SIZE, NULL, NULL)) return false;
    }
    return true;
}

static void release_stack(kernel_thread_t *thread) {
    if (thread->stack_phys == 0) return;
    for (uint64_t page = 0; page < KERNEL_THREAD_STACK_PAGES; page++) {
        vmm_unmap(thread->stack_base + (page + 1) * PAGE_SIZE);
    }
    pmm_free_frames(thread->stack_phys, KERNEL_THREAD_STACK_PAGES);
    thread->stack_phys = 0;
}

static void reap_dead(void) {
    uint64_t flags = read_rflags();
    cpu_disable_interrupts();
    for (uint32_t i = 1; i < KERNEL_THREAD_MAX; i++) {
        kernel_thread_t *thread = &g_threads[i];
        if (i == g_current || thread->state != THREAD_DEAD) continue;
        release_stack(thread);
        thread->id = 0;
        thread->rsp = 0;
        thread->wake_tick = 0;
        thread->timed_block = false;
        thread->entry = NULL;
        thread->argument = NULL;
        thread->state = THREAD_FREE;
    }
    if (flags & (1u << 9)) cpu_enable_interrupts();
}

static void wake_expired_threads(void) {
    uint64_t now = pit_ticks();
    for (uint32_t i = 0; i < KERNEL_THREAD_MAX; i++) {
        kernel_thread_t *thread = &g_threads[i];
        if (thread->state != THREAD_BLOCKED || !thread->timed_block ||
            now < thread->wake_tick) continue;
        thread->timed_block = false;
        thread->wake_tick = 0;
        thread->state = THREAD_READY;
    }
}

bool thread_system_init(uint64_t hhdm_offset) {
    if (g_ready || hhdm_offset == 0) return false;
    memset(g_threads, 0, sizeof(g_threads));
    g_hhdm = hhdm_offset;
    g_current = 0;
    g_next_id = 1;
    g_switches = 0;
    g_preemptions = 0;
    g_threads[0].id = g_next_id++;
    g_threads[0].state = THREAD_RUNNING;
    g_ready = true;
    return true;
}

thread_id_t thread_create(thread_entry_t entry, void *argument) {
    if (!g_ready || entry == NULL) return 0;

    irq_guard_t guard = irq_guard_enter();
    if (g_next_id == 0) {
        irq_guard_leave(&guard);
        return 0;
    }
    uint32_t slot;
    for (slot = 1; slot < KERNEL_THREAD_MAX; slot++) {
        if (g_threads[slot].state == THREAD_FREE) break;
    }
    if (slot == KERNEL_THREAD_MAX) {
        irq_guard_leave(&guard);
        return 0;
    }
    kernel_thread_t *thread = &g_threads[slot];
    thread->state = THREAD_BUILDING;
    thread->id = g_next_id++;
    uint32_t id = thread->id;
    irq_guard_leave(&guard);

    uint64_t base = THREAD_STACK_ARENA +
        (uint64_t)slot * THREAD_STACK_STRIDE * PAGE_SIZE;
    if (!stack_region_clear(base)) goto fail;
    uintptr_t phys = pmm_alloc_frames(KERNEL_THREAD_STACK_PAGES);
    if (phys == 0) goto fail;

    uint64_t mapped = 0;
    for (; mapped < KERNEL_THREAD_STACK_PAGES; mapped++) {
        uint64_t virt = base + (mapped + 1) * PAGE_SIZE;
        uintptr_t frame = phys + mapped * PAGE_SIZE;
        if (!vmm_map(virt, frame, VMM_PRESENT | VMM_WRITE | VMM_NX)) break;
        memset((void *)(uintptr_t)(frame + g_hhdm), 0, PAGE_SIZE);
    }
    if (mapped != KERNEL_THREAD_STACK_PAGES) {
        while (mapped != 0) {
            mapped--;
            vmm_unmap(base + (mapped + 1) * PAGE_SIZE);
        }
        pmm_free_frames(phys, KERNEL_THREAD_STACK_PAGES);
        goto fail;
    }

    thread->stack_base = base;
    thread->stack_phys = phys;
    thread->entry = entry;
    thread->argument = argument;

    /* New threads start from the same full long-mode interrupt frame saved
     * for a preempted thread. Keep the C entry stack 8 mod 16 per the ABI.
     */
    uint64_t stack_top = base + THREAD_STACK_STRIDE * PAGE_SIZE;
    registers_t *frame = (registers_t *)(uintptr_t)
        (stack_top - sizeof(registers_t));
    memset(frame, 0, sizeof(*frame));
    frame->vector = 48;
    frame->rip = (uint64_t)(uintptr_t)thread_bootstrap;
    frame->cs = read_cs();
    frame->rflags = 0x202; /* reserved bit 1 + interrupts enabled */
    frame->rsp = stack_top - sizeof(uintptr_t);
    frame->ss = read_ss();
    thread->rsp = (uint64_t)(uintptr_t)frame;
    guard = irq_guard_enter();
    thread->state = THREAD_READY;
    irq_guard_leave(&guard);
    return id;

fail:
    guard = irq_guard_enter();
    memset(thread, 0, sizeof(*thread));
    irq_guard_leave(&guard);
    return 0;
}

bool thread_yield(void) {
    if (!g_ready) return false;
    uint64_t before = g_switches;
    __asm__ volatile("int $48" : : : "memory", "cc");
    /* This is safe after iretq has restored this thread's own stack. */
    reap_dead();
    return g_switches != before;
}

bool thread_block_current(void) {
    if (!g_ready) return false;
    irq_guard_t guard = irq_guard_enter();
    bool runnable = false;
    for (uint32_t i = 0; i < KERNEL_THREAD_MAX; i++) {
        if (i != g_current && g_threads[i].state == THREAD_READY) {
            runnable = true;
            break;
        }
    }
    if (!runnable || g_threads[g_current].state != THREAD_RUNNING) {
        irq_guard_leave(&guard);
        return false;
    }

    g_threads[g_current].state = THREAD_BLOCKED;
    __asm__ volatile("int $48" : : : "memory", "cc");
    irq_guard_leave(&guard);
    return true;
}

bool thread_block_current_until_tick(uint64_t deadline_tick) {
    if (!g_ready) return false;
    irq_guard_t guard = irq_guard_enter();
    if (deadline_tick <= pit_ticks() ||
        g_threads[g_current].state != THREAD_RUNNING) {
        irq_guard_leave(&guard);
        return false;
    }
    bool runnable = false;
    for (uint32_t i = 0; i < KERNEL_THREAD_MAX; i++) {
        if (i != g_current && g_threads[i].state == THREAD_READY) {
            runnable = true;
            break;
        }
    }
    if (!runnable) {
        irq_guard_leave(&guard);
        return false;
    }

    kernel_thread_t *thread = &g_threads[g_current];
    thread->wake_tick = deadline_tick;
    thread->timed_block = true;
    thread->state = THREAD_BLOCKED;
    __asm__ volatile("int $48" : : : "memory", "cc");
    irq_guard_leave(&guard);
    return true;
}

bool thread_wake(thread_id_t id) {
    IRQ_GUARD;
    if (!g_ready || id == 0) return false;
    for (uint32_t i = 0; i < KERNEL_THREAD_MAX; i++) {
        kernel_thread_t *thread = &g_threads[i];
        if (thread->id != id || thread->state != THREAD_BLOCKED) continue;
        thread->state = THREAD_READY;
        thread->timed_block = false;
        thread->wake_tick = 0;
        return true;
    }
    return false;
}

thread_id_t thread_current_id(void) {
    IRQ_GUARD;
    return g_ready ? g_threads[g_current].id : 0;
}

__attribute__((noreturn)) void thread_exit(void) {
    if (!g_ready || g_current == 0) panic("bootstrap thread cannot exit");
    g_threads[g_current].state = THREAD_DEAD;
    __asm__ volatile("int $48" : : : "memory", "cc");
    panic("dead thread resumed");
}

registers_t *thread_schedule(registers_t *interrupted, bool timer_tick) {
    if (!g_ready || interrupted == NULL) return interrupted;
    if (timer_tick) wake_expired_threads();
    /* A dead thread's handler is still running on its stack. Reap only dead
     * threads other than the interrupted one, before choosing the next task.
     */
    reap_dead();

    uint32_t previous_index = g_current;
    kernel_thread_t *previous = &g_threads[previous_index];
    if (previous->state == THREAD_RUNNING) {
        previous->rsp = (uint64_t)(uintptr_t)interrupted;
        previous->state = THREAD_READY;
    } else if (previous->state == THREAD_BLOCKED) {
        previous->rsp = (uint64_t)(uintptr_t)interrupted;
    }

    uint32_t next_index = previous_index;
    for (uint32_t offset = 1; offset <= KERNEL_THREAD_MAX; offset++) {
        uint32_t candidate = (previous_index + offset) % KERNEL_THREAD_MAX;
        if (g_threads[candidate].state == THREAD_READY) {
            next_index = candidate;
            break;
        }
    }
    if (next_index == previous_index) {
        if (previous->state == THREAD_DEAD) {
            panic("no runnable thread after thread exit");
        }
        if (previous->state == THREAD_BLOCKED) {
            panic("no runnable thread after thread block");
        }
        previous->state = THREAD_RUNNING;
        return interrupted;
    }

    kernel_thread_t *next = &g_threads[next_index];
    next->state = THREAD_RUNNING;
    g_current = next_index;
    g_switches++;
    if (timer_tick) g_preemptions++;
    return (registers_t *)(uintptr_t)next->rsp;
}

uint32_t thread_live_count(void) {
    IRQ_GUARD;
    uint32_t count = 0;
    if (!g_ready) return 0;
    for (uint32_t i = 0; i < KERNEL_THREAD_MAX; i++) {
        if (g_threads[i].state == THREAD_READY ||
            g_threads[i].state == THREAD_RUNNING ||
            g_threads[i].state == THREAD_BLOCKED) count++;
    }
    return count;
}

uint64_t thread_switch_count(void) {
    IRQ_GUARD;
    return g_switches;
}
uint64_t thread_preemption_count(void) {
    IRQ_GUARD;
    return g_preemptions;
}
