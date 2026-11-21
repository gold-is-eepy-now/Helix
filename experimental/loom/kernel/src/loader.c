/* Loading Cells from flat images. A Cell gets its own address space: its image mapped at 0x400000
 * (one page per 4 KiB), a 16 KiB stack below 0x704000, and the capabilities the kernel gives it.
 * cell_destroy frees every frame the Cell's space used. */
#include <stdint.h>
#include <string.h>
#include "arch.h"
#include "loom_cell.h"
#include "loom_kernel.h"
#include <assert.h>

#define CELL_CODE_VA    0x400000ull
#define CELL_STACK_VA   0x700000ull
#define CELL_STACK_PAGES 4
#define CELL_MAX_PAGES  16

bool cell_create(struct loom_cell *c, const uint8_t *start, const uint8_t *end, const struct cap *caps) {
    size_t len = (size_t)(end - start);
    size_t pages = (len + 4095) / 4096;
    assert(pages > 0 && pages <= CELL_MAX_PAGES);

    memset(c, 0, sizeof *c);
    uint64_t space = vm_new_space();
    if (space == 0) return false;
    for (size_t i = 0; i < pages; i++) {
        uint64_t frame = pmm_alloc_frame();
        if (frame == 0) return false;
        size_t off = i * 4096;
        size_t n = len - off < 4096 ? len - off : 4096;
        memset((void *)phys_to_virt(frame), 0, 4096);
        memcpy((void *)phys_to_virt(frame), start + off, n);
        if (!vm_map_user(space, CELL_CODE_VA + off, frame, PTE_USER_RX)) return false;
    }
    for (size_t i = 0; i < CELL_STACK_PAGES; i++) {
        uint64_t frame = pmm_alloc_frame();
        if (frame == 0) return false;
        memset((void *)phys_to_virt(frame), 0, 4096);
        if (!vm_map_user(space, CELL_STACK_VA + i * 4096, frame, PTE_USER_RW)) return false;
    }

    c->ctx.user_cr3 = space;
    c->ctx.entry = CELL_CODE_VA;
    c->ctx.user_rsp = CELL_STACK_VA + CELL_STACK_PAGES * 4096 - 16;
    if (caps) memcpy(c->caps, caps, sizeof c->caps);
    return true;
}

void cell_destroy(struct loom_cell *c) {
    assert(c->ctx.user_cr3 != vm_current_cr3());
    vm_free_space(c->ctx.user_cr3);
    c->ctx.user_cr3 = 0;
}

/* One run to completion: create, run, destroy. */
int64_t cell_spawn_run(const uint8_t *start, const uint8_t *end, const struct cap *caps) {
    struct loom_cell c;
    bool ok = cell_create(&c, start, end, caps);
    assert(ok);
    int64_t status = cell_run(&c.ctx);
    cell_destroy(&c);
    return status;
}
