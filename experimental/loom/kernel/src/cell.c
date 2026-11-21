/* Running Cells: the kernel side of cell_enter / cell_leave, and the trap dispatcher.
 *
 * A Cell runs until it exits or faults. Nothing preempts it (interrupts stay off), so there is one
 * Cell on the CPU at a time. Traps from ring 3 are either system calls (vector 0x80) or faults, and
 * a fault kills the Cell. A trap from ring 0 is a kernel bug and panics. */
#include <stdint.h>
#include "arch.h"
#include "loom_kernel.h"

#define CELL_FAULT_BASE  0x1000   /* exit status for a Cell killed by exception v: 0x1000 + v */

static struct cell_ctx *g_cur;
int64_t cell_enter_frame(struct cell_ctx *c, struct trap_frame *f);   /* trap.S */
static uint8_t g_trap_stack[16384] __attribute__((aligned(16)));

int64_t cell_run(struct cell_ctx *c) {
    g_cur = c;
    tss_set_rsp0((uint64_t)(g_trap_stack + sizeof g_trap_stack));   /* traps from the Cell land here */
    return cell_enter(c);
}

struct cell_ctx *cell_current(void) { return g_cur; }

int64_t cell_resume(struct cell_ctx *c, struct trap_frame *f) {
    g_cur = c;
    tss_set_rsp0((uint64_t)(g_trap_stack + sizeof g_trap_stack));
    return cell_enter_frame(c, f);
}

void syscall_dispatch(struct trap_frame *f);   /* syscall.c */

static uint64_t read_cr2(void) {
    uint64_t v;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(v));
    return v;
}

void trap_dispatch(struct trap_frame *f) {
    bool from_user = (f->cs & 3) == 3;

    if (f->vector == 0x80) {
        if (!from_user) kpanic("system call from kernel mode");
        syscall_dispatch(f);
        return;
    }

    if (from_user) {
        serial_puts("cell killed: exception ");
        serial_putu64(f->vector);
        serial_puts(" at rip ");
        serial_puthex64(f->rip);
        if (f->vector == 14) {
            serial_puts(", address ");
            serial_puthex64(read_cr2());
            serial_puts(", error ");
            serial_putu64(f->error);
        }
        serial_puts("\n");
        cell_leave(g_cur, (int64_t)(CELL_FAULT_BASE + f->vector));
    }

    serial_puts("LOOM kernel exception vector ");
    serial_putu64(f->vector);
    serial_puts(" rip ");
    serial_puthex64(f->rip);
    serial_puts(" cr2 ");
    serial_puthex64(read_cr2());
    serial_puts("\n");
    kpanic("unexpected exception in kernel mode");
}
