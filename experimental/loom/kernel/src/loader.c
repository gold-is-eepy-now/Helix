/* Loading a Cell from a flat image. Maps the image at 0x400000 (one page per 4 KiB of image), one
 * zeroed stack page, and runs it in its own address space. The address space is torn down after the
 * run, so every frame it used comes back. */
#include <stdint.h>
#include <string.h>
#include "arch.h"
#include "loom_cell.h"
#include "loom_kernel.h"
#include <assert.h>

#define CELL_CODE_VA   0x400000ull
#define CELL_STACK_VA 0x700000ull
#define CELL_MAX_PAGES 16

int64_t cell_spawn_run(const uint8_t *start, const uint8_t *end, const struct cap caps[CELL_MAX_CAPS]) {
    size_t len = (size_t)(end - start);
    size_t pages = (len + 4095) / 4096;
    assert(pages > 0 && pages <= CELL_MAX_PAGES);

    uint64_t space = vm_new_space();
    assert(space != 0);
    for (size_t i = 0; i < pages; i++) {
        uint64_t frame = pmm_alloc_frame();
        assert(frame != 0);
        size_t off = i * 4096;
        size_t n = len - off < 4096 ? len - off : 4096;
        memset((void *)phys_to_virt(frame), 0, 4096);
        memcpy((void *)phys_to_virt(frame), start + off, n);
        bool ok = vm_map_user(space, CELL_CODE_VA + off, frame, PTE_USER_RX);
        assert(ok);
    }
    uint64_t stack = pmm_alloc_frame();
    assert(stack != 0);
    memset((void *)phys_to_virt(stack), 0, 4096);
    bool ok = vm_map_user(space, CELL_STACK_VA, stack, PTE_USER_RW);
    assert(ok);

    struct loom_cell c;
    memset(&c, 0, sizeof c);
    c.ctx.user_cr3 = space;
    c.ctx.entry = CELL_CODE_VA;
    c.ctx.user_rsp = CELL_STACK_VA + 4096 - 16;
    if (caps) memcpy(c.caps, caps, sizeof c.caps);

    int64_t status = cell_run(&c.ctx);
    vm_free_space(space);
    return status;
}
