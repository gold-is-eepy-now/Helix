/* usermode.c — a first ring-3 process: its own address space, one code page, one
 * stack page, and an exit path back to the kernel. Used by the `usermode` shell
 * command. Programs are hand-assembled for now; ELF loading comes later. */
#include "usermode.h"

#include <stddef.h>
#include <stdint.h>

#include "console.h"
#include "gdt.h"
#include "kstring.h"
#include "pmm.h"
#include "vmm.h"

/* Defined in usermode.S. */
extern uint64_t usermode_run(uint64_t entry, uint64_t user_rsp, uint64_t *save,
                             uint64_t user_cs, uint64_t user_ss);
extern void usermode_return(uint64_t *save, uint64_t status) __attribute__((noreturn));

#define USER_CODE_VA  0x400000ULL
#define USER_STACK_VA 0x800000ULL   /* one page; the stack grows down from its top */

/* Machine code for the test program. Layout:
 *   mov eax, 26                  ; SYSCALL_DEBUG_WRITE
 *   lea rdi, [rip + msg]
 *   mov esi, 18
 *   int 0x80
 *   mov eax, 23                  ; SYSCALL_EXIT
 *   mov edi, 7
 *   int 0x80
 *   jmp $                        ; not reached
 *   msg: "hello from ring 3\n"
 */
static const uint8_t k_user_program[] = {
    0xB8, 0x1A, 0x00, 0x00, 0x00,       /* mov eax, 26          */
    0x48, 0x8D, 0x3D, 0x15, 0x00, 0x00, 0x00, /* lea rdi, [rip+21] */
    0xBE, 0x12, 0x00, 0x00, 0x00,       /* mov esi, 18          */
    0xCD, 0x80,                         /* int 0x80             */
    0xB8, 0x17, 0x00, 0x00, 0x00,       /* mov eax, 23          */
    0xBF, 0x07, 0x00, 0x00, 0x00,       /* mov edi, 7           */
    0xCD, 0x80,                         /* int 0x80             */
    0xEB, 0xFE,                         /* jmp $                */
    'h', 'e', 'l', 'l', 'o', ' ', 'f', 'r', 'o', 'm', ' ',
    'r', 'i', 'n', 'g', ' ', '3', '\n',
};

static uint64_t g_save[8];
static bool g_active;
static vmm_as_t g_kernel_as;

bool usermode_is_active(void) {
    return g_active;
}

void usermode_exit(uint64_t status) {
    g_active = false;
    usermode_return(g_save, status);
}

int64_t usermode_run_test(void) {
    g_kernel_as = *vmm_current_as();

    uintptr_t code = pmm_alloc_frame();
    uintptr_t stack = pmm_alloc_frame();
    if (code == 0 || stack == 0) return -1;

    memcpy(vmm_phys_to_virt(code), k_user_program, sizeof(k_user_program));
    memset(vmm_phys_to_virt(stack), 0, 4096);

    vmm_as_t *as = vmm_create_as();
    if (as == NULL) return -1;

    vmm_switch_as(as);
    bool mapped = vmm_map(USER_CODE_VA, (uint64_t)code, VMM_PRESENT | VMM_USER)
               && vmm_map(USER_STACK_VA, (uint64_t)stack,
                          VMM_PRESENT | VMM_WRITE | VMM_USER | VMM_NX);
    if (!mapped) {
        vmm_switch_as(&g_kernel_as);
        return -1;
    }

    g_active = true;
    int64_t status = (int64_t)usermode_run(USER_CODE_VA, USER_STACK_VA + 4096u, g_save,
                                           gdt_user_code_selector(),
                                           gdt_user_data_selector());
    /* Reached through usermode_exit: back on the kernel's address space. */
    vmm_switch_as(&g_kernel_as);
    return status;
}
