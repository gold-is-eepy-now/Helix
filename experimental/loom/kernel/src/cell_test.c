/* Cell tests: two Cells, each in its own address space. Checks the exit status, that a fault
 * kills only the faulting Cell, and that the frames each run used are returned afterwards. */
#include <stdint.h>
#include <string.h>
#include "arch.h"
#include "loom_kernel.h"
#include <assert.h>

#define CELL_CODE_VA  0x400000ull
#define CELL_STACK_VA 0x700000ull     /* one page; the Cell's stack grows down from its top */

extern const uint8_t cell_exit_start[], cell_exit_end[];
extern const uint8_t cell_fault_start[], cell_fault_end[];

static void report(const char *what, bool ok) {
    serial_puts(ok ? "PASS: " : "FAIL: ");
    serial_puts(what);
    serial_puts("\n");
}

/* Builds an address space holding the code image and a stack, runs it, and tears it down.
 * Returns the Cell's exit status, or a negative value if the space could not be built. */
static int64_t run_image(const uint8_t *start, const uint8_t *end) {
    uint64_t space = vm_new_space();
    uint64_t code = pmm_alloc_frame(), stack = pmm_alloc_frame();
    if (!space || !code || !stack) return -1;

    size_t len = (size_t)(end - start);
    assert(len <= 4096);
    memcpy((void *)phys_to_virt(code), start, len);
    memset((void *)phys_to_virt(stack), 0, 4096);

    bool ok = vm_map_user(space, CELL_CODE_VA, code, PTE_USER_RX) &&
              vm_map_user(space, CELL_STACK_VA, stack, PTE_USER_RW);
    if (!ok) return -2;

    struct cell_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.user_cr3 = space;
    ctx.entry = CELL_CODE_VA;
    ctx.user_rsp = CELL_STACK_VA + 4096 - 16;

    int64_t status = cell_run(&ctx);
    vm_free_space(space);     /* returns the code and stack frames too */
    return status;
}

bool cell_selftest(void) {
    bool all = true;
    uint64_t free0 = pmm_free_count();

    int64_t exit_status = run_image(cell_exit_start, cell_exit_end);
    bool exit_ok = exit_status == 7;
    report("cell exits through SYS_EXIT with its status (7)", exit_ok); all &= exit_ok;

    int64_t fault_status = run_image(cell_fault_start, cell_fault_end);
    bool fault_ok = fault_status == 0x1000 + 14;
    report("cell touching an unmapped address is killed (page fault)", fault_ok); all &= fault_ok;

    /* After a fault, the kernel must still run Cells correctly: the clean cell runs again. */
    int64_t after_fault = run_image(cell_exit_start, cell_exit_end);
    bool kernel_alive = after_fault == 7;
    report("a clean cell still exits with 7 after another cell was killed", kernel_alive); all &= kernel_alive;

    bool no_leak = pmm_free_count() == free0;
    report("both cells leave no frames behind", no_leak); all &= no_leak;
    return all;
}
