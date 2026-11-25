/* vm_test.c — i686 stage I2b self-tests: 4 KiB mappings through PAE paging, and the heap.
 *
 * Paging: map a virtual page outside the identity map (1 GiB), write through it, read the
 * same frame through its identity address, then unmap and check the entry is gone.
 * Heap: two blocks are distinct and writable, and freeing them restores the used count. */
#include <stdbool.h>
#include <stdint.h>

#include "heap.h"
#include "paging.h"
#include "pmm.h"

#define TEST_VIRT 0x40000000u       /* 1 GiB: not covered by the identity map */

bool paging_selftest(void) {
    uintptr_t frame = pmm_alloc_frame();
    if (frame == 0) return false;

    if (!paging_map4k(TEST_VIRT, frame, PAGE_WRITABLE)) return false;
    if (paging_map4k(TEST_VIRT, frame, PAGE_WRITABLE)) return false; /* double map refused */

    volatile uint32_t *via_virt = (volatile uint32_t *)(uintptr_t)TEST_VIRT;
    volatile uint32_t *via_phys = (volatile uint32_t *)frame;
    *via_virt = 0xC0FFEE42u;
    if (*via_phys != 0xC0FFEE42u) return false;

    if (!paging_unmap4k(TEST_VIRT)) return false;
    if (paging_pte(TEST_VIRT) != 0) return false;
    pmm_free_frame(frame);
    return true;
}

bool heap_selftest(void) {
    uint64_t used0 = heap_used_bytes();
    unsigned char *a = kmalloc(100);
    unsigned char *b = kmalloc(100);
    if (a == NULL || b == NULL || a == b) return false;
    for (int i = 0; i < 100; i++) {
        a[i] = 0xA5;
        b[i] = 0x5A;
    }
    for (int i = 0; i < 100; i++) {
        if (a[i] != 0xA5 || b[i] != 0x5A) return false;
    }
    kfree(a);
    kfree(b);
    return heap_used_bytes() == used0;
}
