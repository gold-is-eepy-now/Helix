/* Memory self-test: frames and page tables, checked in place. */
#include <stdint.h>
#include <string.h>
#include "loom_kernel.h"
#include <assert.h>

static void say_result(const char *what, bool ok) {
    serial_puts(ok ? "PASS: " : "FAIL: ");
    serial_puts(what);
    serial_puts("\n");
}

bool mm_selftest(void) {
    bool all = true;
    uint64_t free0 = pmm_free_count();

    uint64_t a = pmm_alloc_frame(), b = pmm_alloc_frame(), c = pmm_alloc_frame();
    bool distinct = a && b && c && a != b && b != c && a != c;
    bool aligned = ((a | b | c) & 0xFFF) == 0;
    say_result("three distinct, page-aligned frames", distinct && aligned); all &= distinct && aligned;
    pmm_free_frame(a); pmm_free_frame(b); pmm_free_frame(c);
    bool restored = pmm_free_count() == free0;
    say_result("freeing the frames restores the free count", restored); all &= restored;

    uint64_t space = vm_new_space();
    bool made = space != 0;
    say_result("new address space created", made); all &= made;
    if (!made) return false;

    uint64_t page = pmm_alloc_frame();
    const uint64_t virt = 0x400000ull;
    bool mapped = vm_map_user(space, virt, page, PTE_USER_RW);
    say_result("user page mapped at 0x400000", mapped); all &= mapped;
    bool translates = vm_translate(space, virt) == page;
    say_result("translation of the mapped page is correct", translates); all &= translates;
    bool twice = !vm_map_user(space, virt, page, PTE_USER_RW);
    say_result("mapping an already-mapped address is refused", twice); all &= twice;
    bool kernel_hidden = vm_translate(space, 0x600000ull) == 0;
    say_result("an unmapped address does not translate", kernel_hidden); all &= kernel_hidden;

    uint64_t back = vm_unmap_user(space, virt);
    bool unmapped = back == page && vm_translate(space, virt) == 0;
    say_result("unmap returns the frame and removes the translation", unmapped); all &= unmapped;
    pmm_free_frame(page);

    vm_free_space(space);
    bool no_leak = pmm_free_count() == free0;
    say_result("destroying the address space leaves no frames behind", no_leak); all &= no_leak;
    return all;
}
