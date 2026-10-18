/* usermode.c — minimal ring-3 entry/exit proof before ELF and syscall work. */
#include "usermode.h"

#include <stddef.h>

#include "console.h"
#include "gdt.h"
#include "io.h"
#include "input.h"
#include "kstring.h"
#include "pmm.h"
#include "syscall.h"
#include "sync.h"
#include "vmm.h"

#define USER_CODE_VIRT  0x00400000ULL
#define USER_STACK_VIRT 0x00401000ULL
#define USER_STACK_TOP  (USER_STACK_VIRT + PAGE_SIZE)
#define USER_EXIT_VECTOR 128u
#define TRANSITION_STACK_GUARD 0xffff900000400000ULL
#define TRANSITION_STACK_BASE  (TRANSITION_STACK_GUARD + PAGE_SIZE)
#define TRANSITION_STACK_PAGES 4u
#define TRANSITION_STACK_TOP \
    (TRANSITION_STACK_BASE + TRANSITION_STACK_PAGES * PAGE_SIZE)

uint64_t usermode_saved_rsp;
static bool g_exit_armed;
static bool g_exit_received;
static uint64_t g_result;
static uint64_t g_fault_vector;
static uintptr_t g_transition_stack_phys;
static vmm_address_space_t *g_active_space;
typedef struct { uint64_t kernel_rsp; uint64_t user_rsp; } syscall_cpu_t;
static syscall_cpu_t g_syscall_cpu __attribute__((aligned(16)));

extern void usermode_enter(uint64_t entry, uint64_t user_rsp);
extern void usermode_kernel_return(void);
extern void usermode_syscall_entry(void);

bool usermode_syscall_init(void) {
    const uint32_t cpuid_leaf = 0x80000001u;
    uint32_t max_ext, b, c, d;
    __asm__ volatile("cpuid" : "=a"(max_ext), "=b"(b), "=c"(c), "=d"(d)
                     : "a"(0x80000000u), "c"(0));
    if (max_ext < cpuid_leaf) return false;
    __asm__ volatile("cpuid" : "=a"(max_ext), "=b"(b), "=c"(c), "=d"(d)
                     : "a"(cpuid_leaf), "c"(0));
    if ((d & (1u << 11)) == 0) return false; /* SYSCALL/SYSRET */

    const uint32_t efer_msr = 0xC0000080u;
    const uint32_t star_msr = 0xC0000081u;
    const uint32_t lstar_msr = 0xC0000082u;
    const uint32_t fmask_msr = 0xC0000084u;
    const uint32_t gs_base_msr = 0xC0000101u;
    wrmsr(efer_msr, rdmsr(efer_msr) | 1u); /* EFER.SCE */
    wrmsr(star_msr, ((uint64_t)0x13u << 48) | ((uint64_t)0x08u << 32));
    wrmsr(lstar_msr, (uint64_t)(uintptr_t)usermode_syscall_entry);
    wrmsr(fmask_msr, (1u << 8) | (1u << 9) | (1u << 10)); /* TF, IF, DF */
    g_syscall_cpu.kernel_rsp = TRANSITION_STACK_TOP;
    g_syscall_cpu.user_rsp = 0;
    wrmsr(gs_base_msr, (uint64_t)(uintptr_t)&g_syscall_cpu);
    return true;
}

static void redirect_to_kernel(registers_t *frame) {
    frame->rip = (uint64_t)(uintptr_t)usermode_kernel_return;
    frame->cs = GDT_KERNEL_CODE_SELECTOR;
    frame->rflags = 0x2;
    frame->rsp = TRANSITION_STACK_TOP;
    frame->ss = GDT_KERNEL_DATA_SELECTOR;
}

static bool ensure_transition_stack(void) {
    if (g_transition_stack_phys != 0) return true;
    for (uint64_t page = 0; page <= TRANSITION_STACK_PAGES; page++) {
        if (vmm_get_mapping(TRANSITION_STACK_GUARD + page * PAGE_SIZE,
                            NULL, NULL)) return false;
    }
    uintptr_t phys = pmm_alloc_frames(TRANSITION_STACK_PAGES);
    if (phys == 0) return false;
    uint64_t mapped = 0;
    for (; mapped < TRANSITION_STACK_PAGES; mapped++) {
        uint64_t virt = TRANSITION_STACK_BASE + mapped * PAGE_SIZE;
        uintptr_t frame = phys + mapped * PAGE_SIZE;
        if (!vmm_map(virt, frame, VMM_PRESENT | VMM_WRITE | VMM_NX)) break;
        memset((void *)(uintptr_t)virt, 0, PAGE_SIZE);
    }
    if (mapped != TRANSITION_STACK_PAGES) {
        while (mapped != 0) {
            mapped--;
            vmm_unmap(TRANSITION_STACK_BASE + mapped * PAGE_SIZE);
        }
        pmm_free_frames(phys, TRANSITION_STACK_PAGES);
        return false;
    }
    g_transition_stack_phys = phys;
    return true;
}

static bool user_pointer_accessible(uint64_t address, uint64_t length,
                                    uint64_t required_flags) {
    const uint64_t user_top = 0x0000800000000000ULL;
    if (length == 0) return true;
    if (address < PAGE_SIZE || address >= user_top ||
        length > user_top - address) return false;
    uint64_t last = address + length - 1;
    for (uint64_t page = address & ~((uint64_t)PAGE_SIZE - 1);;
         page += PAGE_SIZE) {
        uint64_t flags = 0;
        if (!vmm_address_space_get_mapping(g_active_space, page, NULL,
                                           &flags) ||
            (flags & required_flags) != required_flags)
            return false;
        if (page == (last & ~((uint64_t)PAGE_SIZE - 1))) break;
    }
    return true;
}

bool usermode_handle_syscall(registers_t *frame) {
    if (frame == NULL || frame->vector != USER_EXIT_VECTOR ||
        (frame->cs & 3u) != 3u || !g_exit_armed || g_active_space == NULL)
        return false;

    if (frame->rax == HELIX_SYS_WRITE) {
        uint64_t address = frame->rdi;
        uint64_t length = frame->rsi;
        if (length > HELIX_SYS_MAX_WRITE ||
            !user_pointer_accessible(address, length,
                                     VMM_PRESENT | VMM_USER)) {
            frame->rax = (uint64_t)-1;
            return true;
        }
        const char *text = (const char *)(uintptr_t)address;
        for (uint64_t i = 0; i < length; i++) console_putc(text[i]);
        frame->rax = length;
        return true;
    }
    if (frame->rax == HELIX_SYS_READ) {
        uint64_t address = frame->rdi;
        uint64_t capacity = frame->rsi;
        if (capacity > HELIX_SYS_MAX_READ ||
            !user_pointer_accessible(address, capacity,
                VMM_PRESENT | VMM_USER | VMM_WRITE)) {
            frame->rax = (uint64_t)-1;
            return true;
        }
        char *buffer = (char *)(uintptr_t)address;
        uint64_t length = 0;
        while (length < capacity) {
            int c = input_getchar();
            if (c < 0) {
                __asm__ volatile("pause");
                continue;
            }
            if (c == '\r') c = '\n';
            if ((c == '\b' || c == 0x7f) && length != 0) {
                length--;
                console_backspace();
                continue;
            }
            if (c == '\b' || c == 0x7f) continue;
            buffer[length++] = (char)c;
            console_putc((char)c);
            if (c == '\n') break;
        }
        frame->rax = length;
        return true;
    }
    if (frame->rax == HELIX_SYS_CLEAR) {
        console_clear();
        frame->rax = 0;
        return true;
    }
    if (frame->rax != HELIX_SYS_EXIT) {
        frame->rax = (uint64_t)-1;
        return true;
    }

    g_result = (uint32_t)frame->rdi;
    g_exit_received = true;
    g_exit_armed = false;
    if (!vmm_address_space_switch(vmm_kernel_address_space())) return false;
    g_active_space = NULL;

    /* IRETQ returns to a kernel trampoline, which restores the saved call
     * stack; same-privilege return does not consume the RSP/SS pair. */
    redirect_to_kernel(frame);
    return true;
}

bool usermode_test(uint64_t *result_out) {
    if (result_out == NULL) return false;
    IRQ_GUARD;

    if (!ensure_transition_stack()) return false;

    vmm_address_space_t space = {0};
    uintptr_t code_frame = 0;
    uintptr_t stack_frame = 0;
    bool code_mapped = false;
    bool stack_mapped = false;
    bool entered = false;
    bool ok = vmm_address_space_create(&space);
    if (!ok) goto cleanup;

    code_frame = pmm_alloc_frame();
    stack_frame = pmm_alloc_frame();
    if (code_frame == 0 || stack_frame == 0) {
        ok = false;
        goto cleanup;
    }
    code_mapped = vmm_address_space_map_user(&space, USER_CODE_VIRT,
        code_frame, VMM_PRESENT | VMM_WRITE | VMM_USER | VMM_NX);
    stack_mapped = vmm_address_space_map_user(&space, USER_STACK_VIRT,
        stack_frame, VMM_PRESENT | VMM_WRITE | VMM_USER | VMM_NX);
    if (!code_mapped || !stack_mapped) {
        ok = false;
        goto cleanup;
    }

    /* mov eax, SYS_EXIT; mov edi, 42; int 0x80; ud2. The DPL-3 gate
     * dispatches the first Helix syscall and redirects back to the kernel.
     */
    static const uint8_t user_program[] = {
        0xB8, 0x00, 0x00, 0x00, 0x00,
        0xBF, 0x2A, 0x00, 0x00, 0x00,
        0xCD, 0x80,
        0x0F, 0x0B,
    };
    if (!vmm_address_space_switch(&space)) {
        ok = false;
        goto cleanup;
    }
    memcpy((void *)(uintptr_t)USER_CODE_VIRT, user_program,
           sizeof(user_program));
    memset((void *)(uintptr_t)USER_STACK_VIRT, 0, PAGE_SIZE);
    bool code_protected = vmm_protect(USER_CODE_VIRT,
                                      VMM_PRESENT | VMM_USER);
    if (!vmm_address_space_switch(vmm_kernel_address_space())) {
        ok = false;
        goto cleanup;
    }
    if (!code_protected) {
        ok = false;
        goto cleanup;
    }

    entered = usermode_run(&space, USER_CODE_VIRT, USER_STACK_TOP - 8,
                           result_out, NULL);
    ok = entered && *result_out == 42;

cleanup:
    g_exit_armed = false;
    if ((read_cr3() & 0x000FFFFFFFFFF000ULL) !=
        (vmm_kernel_address_space()->root_phys & 0x000FFFFFFFFFF000ULL)) {
        vmm_address_space_switch(vmm_kernel_address_space());
    }
    if (code_mapped) vmm_address_space_unmap_user(&space, USER_CODE_VIRT);
    if (stack_mapped) vmm_address_space_unmap_user(&space, USER_STACK_VIRT);
    if (space.root_phys != 0) vmm_address_space_destroy(&space);
    if (code_frame != 0) pmm_free_frame(code_frame);
    if (stack_frame != 0) pmm_free_frame(stack_frame);
    return entered && ok;
}

bool usermode_run(vmm_address_space_t *space, uint64_t entry,
                 uint64_t user_rsp, uint64_t *result_out,
                 uint64_t *fault_vector_out) {
    IRQ_GUARD;
    if (space == NULL || space->root_phys == 0 || entry == 0 ||
        user_rsp == 0 || result_out == NULL || !ensure_transition_stack())
        return false;

    g_result = 0;
    g_fault_vector = 0;
    g_exit_received = false;
    g_exit_armed = true;
    g_syscall_cpu.kernel_rsp = TRANSITION_STACK_TOP;
    if (!gdt_set_rsp0(TRANSITION_STACK_TOP) ||
        !vmm_address_space_switch(space)) {
        g_exit_armed = false;
        return false;
    }

    g_active_space = space;
    usermode_enter(entry, user_rsp);
    g_exit_armed = false;
    if ((read_cr3() & 0x000FFFFFFFFFF000ULL) !=
        (vmm_kernel_address_space()->root_phys & 0x000FFFFFFFFFF000ULL) &&
        !vmm_address_space_switch(vmm_kernel_address_space())) return false;
    g_active_space = NULL;
    if (!g_exit_received) return false;
    *result_out = g_result;
    if (fault_vector_out != NULL) *fault_vector_out = g_fault_vector;
    return true;
}

bool usermode_handle_exception(registers_t *frame) {
    if (frame == NULL || frame->vector >= 32 || (frame->cs & 3u) != 3u ||
        !g_exit_armed || g_active_space == NULL) return false;

    g_fault_vector = frame->vector;
    g_exit_received = true;
    g_exit_armed = false;
    if (!vmm_address_space_switch(vmm_kernel_address_space())) return false;
    g_active_space = NULL;
    redirect_to_kernel(frame);
    return true;
}

bool usermode_fault_test(uint64_t *fault_vector_out) {
    IRQ_GUARD;
    const uint64_t stack_page = 0x00401000ULL;
    const uint64_t stack_top = stack_page + PAGE_SIZE;
    vmm_address_space_t space = {0};
    uintptr_t code_frame = 0;
    uintptr_t stack_frame = 0;
    bool code_mapped = false;
    bool stack_mapped = false;
    bool ok = vmm_address_space_create(&space);
    if (!ok) goto cleanup;

    code_frame = pmm_alloc_frame();
    stack_frame = pmm_alloc_frame();
    if (code_frame == 0 || stack_frame == 0) {
        ok = false;
        goto cleanup;
    }
    code_mapped = vmm_address_space_map_user(&space, USER_CODE_VIRT,
        code_frame, VMM_PRESENT | VMM_WRITE | VMM_USER | VMM_NX);
    stack_mapped = vmm_address_space_map_user(&space, stack_page,
        stack_frame, VMM_PRESENT | VMM_WRITE | VMM_USER | VMM_NX);
    if (!code_mapped || !stack_mapped ||
        !vmm_address_space_switch(&space)) {
        ok = false;
        goto cleanup;
    }

    static const uint8_t invalid_opcode[] = {0x0F, 0x0B};
    memcpy((void *)(uintptr_t)USER_CODE_VIRT, invalid_opcode,
           sizeof(invalid_opcode));
    memset((void *)(uintptr_t)stack_page, 0, PAGE_SIZE);
    bool code_protected = vmm_protect(USER_CODE_VIRT,
                                      VMM_PRESENT | VMM_USER);
    bool restored = vmm_address_space_switch(vmm_kernel_address_space());
    if (!code_protected || !restored) {
        ok = false;
        goto cleanup;
    }

    uint64_t result = 0;
    uint64_t fault_vector = 0;
    ok = usermode_run(&space, USER_CODE_VIRT, stack_top - 8,
                      &result, &fault_vector) && fault_vector == 6;
    if (fault_vector_out != NULL) *fault_vector_out = fault_vector;

cleanup:
    if ((read_cr3() & 0x000FFFFFFFFFF000ULL) !=
        (vmm_kernel_address_space()->root_phys & 0x000FFFFFFFFFF000ULL)) {
        vmm_address_space_switch(vmm_kernel_address_space());
    }
    if (code_mapped) vmm_address_space_unmap_user(&space, USER_CODE_VIRT);
    if (stack_mapped) vmm_address_space_unmap_user(&space, stack_page);
    if (space.root_phys != 0) vmm_address_space_destroy(&space);
    if (code_frame != 0) pmm_free_frame(code_frame);
    if (stack_frame != 0) pmm_free_frame(stack_frame);
    return ok;
}
