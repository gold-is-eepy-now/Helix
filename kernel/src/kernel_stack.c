/* kernel_stack.c — dedicated NX kernel stack with an unmapped lower guard. */
#include "kernel_stack.h"

#include <stddef.h>

#include "cpu.h"
#include "idt.h"
#include "kstring.h"
#include "pmm.h"
#include "vmm.h"

#define STACK_REGION_PAGES (KERNEL_STACK_PAGE_COUNT + 1u)

static uint64_t g_hhdm;
static uintptr_t g_stack_phys;
static bool g_stack_ready;

bool kernel_stack_init(uint64_t hhdm_offset) {
    if (g_stack_ready || hhdm_offset == 0 || !cpu_enable_nx()) return false;
    g_hhdm = hhdm_offset;

    /* Do not overwrite any bootloader or earlier kernel mapping. */
    for (uint64_t i = 0; i < STACK_REGION_PAGES; i++) {
        uint64_t phys;
        if (vmm_get_mapping(KERNEL_STACK_GUARD_VIRT + i * PAGE_SIZE,
                            &phys, NULL)) return false;
    }
    uint64_t phys;
    if (vmm_get_mapping(KERNEL_STACK_NX_PROBE_VIRT, &phys, NULL)) return false;

    g_stack_phys = pmm_alloc_frames(KERNEL_STACK_PAGE_COUNT);
    if (g_stack_phys == 0) return false;

    uint64_t mapped = 0;
    for (; mapped < KERNEL_STACK_PAGE_COUNT; mapped++) {
        uint64_t virt = KERNEL_STACK_BASE + mapped * PAGE_SIZE;
        uintptr_t frame = g_stack_phys + mapped * PAGE_SIZE;
        if (!vmm_map(virt, frame, VMM_PRESENT | VMM_WRITE | VMM_NX)) break;
        memset((void *)(uintptr_t)(frame + g_hhdm), 0, PAGE_SIZE);
    }
    if (mapped != KERNEL_STACK_PAGE_COUNT) {
        while (mapped != 0) {
            mapped--;
            vmm_unmap(KERNEL_STACK_BASE + mapped * PAGE_SIZE);
        }
        pmm_free_frames(g_stack_phys, KERNEL_STACK_PAGE_COUNT);
        g_stack_phys = 0;
        return false;
    }

    g_stack_ready = true;
    return true;
}

bool kernel_stack_active(void) {
    uintptr_t rsp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
    return g_stack_ready && rsp >= KERNEL_STACK_BASE && rsp < KERNEL_STACK_TOP;
}

bool kernel_stack_probe_guard(uint64_t *error_out) {
    if (!g_stack_ready || error_out == NULL) return false;
    if (!idt_expect_page_fault(KERNEL_STACK_GUARD_VIRT,
                               (uint64_t)(uintptr_t)kernel_stack_guard_recovery))
        return false;
    bool caught = kernel_stack_guard_attempt((uintptr_t)KERNEL_STACK_GUARD_VIRT) != 0;
    *error_out = idt_expected_page_fault_error();
    caught = caught && idt_expected_page_fault_caught();
    idt_clear_page_fault_expectation();
    return caught;
}

bool kernel_stack_probe_nx(uint64_t *error_out) {
    if (!g_stack_ready || error_out == NULL) return false;
    uintptr_t frame = pmm_alloc_frame();
    if (frame == 0) return false;
    uint64_t phys;
    bool mapped = vmm_map(KERNEL_STACK_NX_PROBE_VIRT, frame,
                          VMM_PRESENT | VMM_WRITE | VMM_NX);
    if (!mapped) {
        pmm_free_frame(frame);
        return false;
    }

    *(volatile uint8_t *)(uintptr_t)(frame + g_hhdm) = 0xC3; /* RET */
    bool protected = vmm_protect(KERNEL_STACK_NX_PROBE_VIRT,
                                  VMM_PRESENT | VMM_NX) &&
                     vmm_get_mapping(KERNEL_STACK_NX_PROBE_VIRT, &phys, NULL) &&
                     phys == frame;
    bool caught = false;
    if (protected && idt_expect_page_fault(KERNEL_STACK_NX_PROBE_VIRT,
                         (uint64_t)(uintptr_t)kernel_stack_exec_recovery)) {
        caught = kernel_stack_exec_attempt((uintptr_t)KERNEL_STACK_NX_PROBE_VIRT) != 0;
        *error_out = idt_expected_page_fault_error();
    }
    idt_clear_page_fault_expectation();
    vmm_unmap(KERNEL_STACK_NX_PROBE_VIRT);
    pmm_free_frame(frame);
    return protected && caught;
}
