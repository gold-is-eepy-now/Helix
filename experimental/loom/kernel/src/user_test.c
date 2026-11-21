/* Runs a Cell written in C (user/hello.c), built as a flat image and embedded in the kernel. */
#include <stdint.h>
#include "loom_cell.h"
#include "loom_kernel.h"
#include <assert.h>

extern const uint8_t hello_image_start[], hello_image_end[];
int64_t cell_spawn_run(const uint8_t *start, const uint8_t *end, const struct cap caps[CELL_MAX_CAPS]);

bool user_selftest(void) {
    bool all = true;
    uint64_t free0 = pmm_free_count();
    int64_t st = cell_spawn_run(hello_image_start, hello_image_end, 0);
    bool ok = st == 3;
    serial_puts(ok ? "PASS: C cell runs from its own image and exits with 3\n"
                   : "FAIL: C cell did not exit with 3\n");
    all &= ok;
    bool no_leak = pmm_free_count() == free0;
    serial_puts(no_leak ? "PASS: the C cell leaves no frames behind\n"
                        : "FAIL: the C cell left frames behind\n");
    all &= no_leak;
    return all;
}
