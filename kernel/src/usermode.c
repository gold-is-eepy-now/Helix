/* usermode.c — ring-3 processes: each gets its own address space, a stack, and an
 * exit path back to the kernel. Two ways in:
 *   usermode_run_test()      a hand-assembled test program (`usermode` command);
 *   usermode_run_elf(img)    a static ELF64 executable, its PT_LOAD segments
 *                            mapped into the process (`usershell` command).
 * The ELF loader accepts only ET_EXEC x86_64 images that fit the user half. */
#include "usermode.h"

#include <stddef.h>
#include <stdint.h>

#include "console.h"
#include "gdt.h"
#include "heap.h"
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

/* -- ELF64 loader ------------------------------------------------------ */

#define ELF_CLASS64     2
#define ELF_DATA_LSB    1
#define ELF_TYPE_EXEC   2
#define ELF_MACHINE_X86_64 62
#define ELF_PT_LOAD     1
#define ELF_PF_X        0x1
#define ELF_PF_W        0x2
#define ELF_PHDR_SIZE   56
#define ELF_MAX_PHDRS   16
#define ELF_MAX_PAGES   256

#define USER_STACK_PAGES 4
#define USER_STACK_BASE  0x800000ULL   /* stack occupies [base, base + 16 KiB) */
#define USER_STACK_TOP   (USER_STACK_BASE + USER_STACK_PAGES * 4096ULL)
#define USER_VA_LIMIT_ELF 0x0000800000000000ULL

/* Header fields we read. Offsets match the ELF64 specification; we copy them out
 * with memcpy so an unaligned blob cannot fault. */
typedef struct {
    uint8_t  ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t phoff;
    uint64_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
} elf64_ehdr_t;

typedef struct {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t vaddr;
    uint64_t paddr;
    uint64_t filesz;
    uint64_t memsz;
    uint64_t align;
} elf64_phdr_t;

/* One mapped user page, so a page shared by two segments is mapped only once. */
typedef struct {
    uint64_t va;
    uintptr_t frame;
    uint64_t flags;
} user_page_t;

typedef struct {
    user_page_t pages[ELF_MAX_PAGES];
    size_t count;
} user_pages_t;

static bool elf_validate(const uint8_t *img, size_t len, elf64_ehdr_t *eh) {
    if (len < sizeof(elf64_ehdr_t)) return false;
    memcpy(eh, img, sizeof(*eh));
    if (eh->ident[0] != 0x7F || eh->ident[1] != 'E' || eh->ident[2] != 'L' || eh->ident[3] != 'F') return false;
    if (eh->ident[4] != ELF_CLASS64 || eh->ident[5] != ELF_DATA_LSB) return false;
    if (eh->type != ELF_TYPE_EXEC || eh->machine != ELF_MACHINE_X86_64) return false;
    if (eh->phentsize != ELF_PHDR_SIZE || eh->phnum == 0 || eh->phnum > ELF_MAX_PHDRS) return false;
    if (eh->phoff > len || (size_t)eh->phnum * ELF_PHDR_SIZE > len - eh->phoff) return false;
    return eh->entry < USER_VA_LIMIT_ELF;
}

/* Find or create the mapping for the page at `va` in the current address space. */
static bool user_page_get(user_pages_t *up, uint64_t va, uint64_t flags, uint8_t **dst) {
    for (size_t i = 0; i < up->count; i++) {
        if (up->pages[i].va == va) {
            if (flags & VMM_WRITE) up->pages[i].flags |= VMM_WRITE;
            if (!(flags & VMM_NX)) up->pages[i].flags &= ~VMM_NX;
            if (!vmm_protect(va, up->pages[i].flags)) return false;
            *dst = vmm_phys_to_virt(up->pages[i].frame);
            return true;
        }
    }
    if (up->count >= ELF_MAX_PAGES) return false;
    uintptr_t frame = pmm_alloc_frame();
    if (frame == 0) return false;
    uint8_t *virt = vmm_phys_to_virt(frame);
    memset(virt, 0, 4096);
    if (!vmm_map(va, (uint64_t)frame, flags)) return false;
    up->pages[up->count].va = va;
    up->pages[up->count].frame = frame;
    up->pages[up->count].flags = flags;
    up->count++;
    *dst = virt;
    return true;
}

/* Map one PT_LOAD segment: each page gets its file bytes, the rest stays zero (bss). */
static bool load_segment(const uint8_t *img, const elf64_phdr_t *ph, user_pages_t *up) {
    if (ph->filesz > ph->memsz) return false;
    if (ph->vaddr >= USER_VA_LIMIT_ELF || ph->memsz > USER_VA_LIMIT_ELF - ph->vaddr) return false;
    if (ph->memsz == 0) return true;

    uint64_t flags = VMM_PRESENT | VMM_USER;
    if (ph->flags & ELF_PF_W) flags |= VMM_WRITE;
    if (!(ph->flags & ELF_PF_X)) flags |= VMM_NX;

    uint64_t seg_end = ph->vaddr + ph->memsz;
    for (uint64_t page = ph->vaddr & ~0xFFFULL; page < seg_end; page += 4096) {
        uint8_t *dst = NULL;
        if (!user_page_get(up, page, flags, &dst)) return false;

        /* Copy the part of this page that the file provides. */
        uint64_t lo = page > ph->vaddr ? page : ph->vaddr;
        uint64_t hi_page = page + 4096;
        uint64_t file_end = ph->vaddr + ph->filesz;
        uint64_t hi = hi_page < file_end ? hi_page : file_end;
        if (lo < hi) {
            memcpy(dst + (lo - page), img + ph->offset + (lo - ph->vaddr), (size_t)(hi - lo));
        }
    }
    return true;
}

/* Load every PT_LOAD of a validated image into the current address space.
 * Returns the entry point through *entry_out. */
static bool elf_load_segments(const uint8_t *img, size_t len, const elf64_ehdr_t *eh,
                              user_pages_t *up, uint64_t *entry_out) {
    for (uint16_t i = 0; i < eh->phnum; i++) {
        elf64_phdr_t ph;
        memcpy(&ph, img + eh->phoff + (size_t)i * ELF_PHDR_SIZE, sizeof(ph));
        if (ph.type != ELF_PT_LOAD) continue;
        if (ph.offset > len || ph.filesz > len - ph.offset) return false;
        if (!load_segment(img, &ph, up)) return false;
    }
    /* The entry point must lie on a page we mapped as executable. */
    for (size_t i = 0; i < up->count; i++) {
        uint64_t page = eh->entry & ~0xFFFULL;
        if (up->pages[i].va == page && !(up->pages[i].flags & VMM_NX)) {
            *entry_out = eh->entry;
            return true;
        }
    }
    return false;
}

/* Run a static ELF64 image in a new process. Returns its exit status, -1 if the
 * process could not be set up, or -2 if the image is not a loadable executable. */
int64_t usermode_run_elf(const uint8_t *img, size_t len) {
    elf64_ehdr_t eh;
    if (!elf_validate(img, len, &eh)) return -2;

    g_kernel_as = *vmm_current_as();
    vmm_as_t *as = vmm_create_as();
    if (as == NULL) return -1;

    /* The user map is built in a new address space; the kernel's half is shared. */
    vmm_switch_as(as);
    user_pages_t *up = kzalloc(sizeof(*up));
    uint64_t entry = 0;
    bool ok = up != NULL && elf_load_segments(img, len, &eh, up, &entry);
    /* Stack: NX, user-writable, mapped just below the top of its window. */
    for (size_t i = 0; ok && i < USER_STACK_PAGES; i++) {
        uint8_t *dst = NULL;
        ok = user_page_get(up, USER_STACK_BASE + i * 4096ULL,
                           VMM_PRESENT | VMM_WRITE | VMM_USER | VMM_NX, &dst);
    }
    if (!ok) {
        vmm_switch_as(&g_kernel_as);
        if (up != NULL) kfree(up);
        return -1;
    }
    kfree(up);

    g_active = true;
    int64_t status = (int64_t)usermode_run(entry, USER_STACK_TOP,
                                           g_save,
                                           gdt_user_code_selector(),
                                           gdt_user_data_selector());
    vmm_switch_as(&g_kernel_as);
    return status;
}

/* The userland shell, built from userspace/shell and linked into the kernel. */
extern const uint8_t user_shell_elf_start[];
extern const uint8_t user_shell_elf_end[];

int64_t usermode_run_user_shell(void) {
    return usermode_run_elf(user_shell_elf_start,
                            (size_t)(user_shell_elf_end - user_shell_elf_start));
}

/* The init process, built from userspace/init.c and linked into the kernel. */
extern const uint8_t user_init_elf_start[];
extern const uint8_t user_init_elf_end[];

int64_t usermode_run_init(void) {
    return usermode_run_elf(user_init_elf_start,
                            (size_t)(user_init_elf_end - user_init_elf_start));
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
