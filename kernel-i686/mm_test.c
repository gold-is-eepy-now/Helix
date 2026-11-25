/* mm_test.c — i686 stage I2 self-test for the shared physical frame allocator.
 *
 * Checks: frames come back page-aligned, distinct, outside the kernel image, and the free
 * count drops by exactly the number taken and comes back after they are freed. */
#include <stdbool.h>
#include <stdint.h>

#include "pmm.h"

extern char __kernel_start[];
extern char __kernel_end[];

#define TAKE 8

bool mm_selftest(void) {
    const uint64_t kstart = (uint64_t)(uintptr_t)__kernel_start;
    const uint64_t kend = (uint64_t)(uintptr_t)__kernel_end;
    const uint64_t before = pmm_free_frame_count();
    uintptr_t frames[TAKE];

    for (int i = 0; i < TAKE; i++) {
        frames[i] = pmm_alloc_frame();
        if (frames[i] == 0) {
            return false;
        }
        if (frames[i] % PAGE_SIZE != 0) {
            return false;
        }
        if ((uint64_t)frames[i] >= kstart && (uint64_t)frames[i] < kend) {
            return false; /* the allocator handed out the kernel's own image */
        }
        for (int j = 0; j < i; j++) {
            if (frames[j] == frames[i]) {
                return false;
            }
        }
    }
    if (pmm_free_frame_count() != before - TAKE) {
        return false;
    }

    for (int i = 0; i < TAKE; i++) {
        pmm_free_frame(frames[i]);
    }
    return pmm_free_frame_count() == before;
}
