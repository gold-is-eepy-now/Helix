/* System calls from Cells. The number is in rax, arguments in rdi, rsi, rdx, r10; the result goes
 * back in rax. Negative results are errors. Only EXIT exists so far. */
#include <stdint.h>
#include "arch.h"
#include "loom_kernel.h"

#define SYS_EXIT  1

#define ENOSYS_RET  (-38)

struct cell_ctx *cell_current(void);

void syscall_dispatch(struct trap_frame *f) {
    switch (f->rax) {
    case SYS_EXIT:
        cell_leave(cell_current(), (int64_t)f->rdi);   /* does not return to the Cell */
    default:
        f->rax = (uint64_t)(int64_t)ENOSYS_RET;
        return;
    }
}
