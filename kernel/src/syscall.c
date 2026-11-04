/* syscall.c — system call interface for user space.
 *
 * M9 milestone: native system-call entry and exit.
 *
 * The kernel exposes a native ABI through the `syscall` instruction.
 * User processes call into the kernel via the syscall gate in the IDT,
 * then the kernel examines the syscall number and arguments (rax, rdi,
 * rsi, rdx, r10, r8, r9) as per the x86_64 ABI and dispatches to the
 * appropriate implementation.
 *
 * All system call handlers share the same signature (7 uint64_t args),
 * matching the x86_64 ABI where the kernel passes all arguments
 * regardless of arity.
 *
 * The system call table is placed in .syscall_table section for the
 * entry stub to locate.
 */

#include <stddef.h>
#include <stdint.h>

#include "console.h"
#include "heap.h"
#include "object.h"
#include "panic.h"
#include "syscall.h"
#include "thread.h"
#include "channel.h"
#include "usermode.h"
#include "vmm.h"
#include "ext4.h"

/* Per-process system call table. Currently one system table for kernel
 * initialization; M9 will add per-process tables when user threads exist.
 * Placed in .syscall_table section so the entry stub can locate it. */
__attribute__((section(".syscall_table")))
syscall_handler_t syscall_table[64u] = {0};

/* Forward declarations — all use the full 7-argument ABI signature. */
static uint64_t handle_test(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
static uint64_t handle_alloc(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
static uint64_t handle_free(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
static uint64_t handle_map(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
static uint64_t handle_unmap(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
static uint64_t handle_thread_create(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
static uint64_t handle_thread_exit(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
static uint64_t handle_thread_yield(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
static uint64_t handle_channel_send(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
static uint64_t handle_channel_recv(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
static uint64_t handle_handle_create(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
static uint64_t handle_handle_close(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
static uint64_t handle_handle_duplicate(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
static uint64_t handle_handle_get(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
static uint64_t handle_object_lookup(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
static uint64_t handle_object_release(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
static uint64_t handle_object_count(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
static uint64_t handle_object_create(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
static uint64_t handle_object_retain(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
static uint64_t handle_read(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
static uint64_t handle_write(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
static uint64_t handle_read_input(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
static uint64_t handle_exit(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
static uint64_t handle_brk(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
static uint64_t handle_execve(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
static uint64_t handle_debug_write(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);

void syscall_init(void) {
    for (size_t i = 0; i < 64u; i++) {
        syscall_table[i] = NULL;
    }

    syscall_table[SYSCALL_TEST]          = handle_test;
    syscall_table[SYSCALL_ALLOC]         = handle_alloc;
    syscall_table[SYSCALL_FREE]          = handle_free;
    syscall_table[SYSCALL_MAP]           = handle_map;
    syscall_table[SYSCALL_UNMAP]         = handle_unmap;
    syscall_table[SYSCALL_THREAD_CREATE] = handle_thread_create;
    syscall_table[SYSCALL_THREAD_EXIT]   = handle_thread_exit;
    syscall_table[SYSCALL_THREAD_YIELD]  = handle_thread_yield;
    syscall_table[SYSCALL_CHANNEL_SEND]  = handle_channel_send;
    syscall_table[SYSCALL_CHANNEL_RECV]  = handle_channel_recv;
    syscall_table[SYSCALL_HANDLE_CREATE] = handle_handle_create;
    syscall_table[SYSCALL_HANDLE_CLOSE]  = handle_handle_close;
    syscall_table[SYSCALL_HANDLE_DUPLICATE] = handle_handle_duplicate;
    syscall_table[SYSCALL_HANDLE_GET]    = handle_handle_get;
    syscall_table[SYSCALL_OBJECT_LOOKUP] = handle_object_lookup;
    syscall_table[SYSCALL_OBJECT_RELEASE] = handle_object_release;
    syscall_table[SYSCALL_OBJECT_COUNT]  = handle_object_count;
    syscall_table[SYSCALL_OBJECT_CREATE] = handle_object_create;
    syscall_table[SYSCALL_OBJECT_RETAIN] = handle_object_retain;
    syscall_table[SYSCALL_READ]          = handle_read;
    syscall_table[SYSCALL_WRITE]         = handle_write;
    syscall_table[SYSCALL_READ_INPUT]    = handle_read_input;
    syscall_table[SYSCALL_EXIT]          = handle_exit;
    syscall_table[SYSCALL_BRK]           = handle_brk;
    syscall_table[SYSCALL_EXECVE]        = handle_execve;
    syscall_table[SYSCALL_DEBUG_WRITE]   = handle_debug_write;
}

/* -- Handler implementations ------------------------------------------ */

static uint64_t handle_test(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    kprintf("[syscall] test arg1=%llu\n", a1);
    return 0;
}

static uint64_t handle_alloc(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    void *ptr = kmalloc(a1);
    if (ptr == NULL) return UINT64_MAX;
    kprintf("[syscall] alloc %llu bytes at %p\n", a1, ptr);
    return (uint64_t)(uintptr_t)ptr;
}

static uint64_t handle_free(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    kfree((void *)(uintptr_t)a1);
    kprintf("[syscall] free %p\n", (void *)(uintptr_t)a1);
    return 0;
}

static uint64_t handle_map(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; (void)a3;
    if (!vmm_map(a1, a2, VMM_PRESENT | VMM_WRITE)) {
        return UINT64_MAX;
    }
    kprintf("[syscall] map %p -> %p\n", a1, a2);
    return 0;
}

static uint64_t handle_unmap(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    if (!vmm_unmap(a1)) {
        return UINT64_MAX;
    }
    kprintf("[syscall] unmap %p\n", a1);
    return 0;
}

static uint64_t handle_thread_create(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    thread_t *t = thread_create((void (*)(void))(uintptr_t)a1);
    if (t == NULL) return UINT64_MAX;
    kprintf("[syscall] thread created\n");
    return (uint64_t)(uintptr_t)t;
}

static uint64_t handle_thread_exit(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    kprintf("[syscall] thread exiting\n");
    thread_exit_current();
    return 0;
}

static uint64_t handle_thread_yield(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    thread_yield();
    return 0;
}

static uint64_t handle_channel_send(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    channel_send((channel_t *)(uintptr_t)a1, a2);
    return 0;
}

static uint64_t handle_channel_recv(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    if (!channel_recv((channel_t *)(uintptr_t)a1, (uint64_t *)a2)) {
        return UINT64_MAX;
    }
    return 0;
}

static uint64_t handle_handle_create(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    handle_t h = handle_create(object_kernel_handles(),
                               object_lookup(a1), (uint32_t)a2);
    if (h == 0) return UINT64_MAX;
    return h;
}

static uint64_t handle_handle_close(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    if (!handle_close(object_kernel_handles(), (handle_t)a1)) {
        return UINT64_MAX;
    }
    return 0;
}

static uint64_t handle_handle_duplicate(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    handle_t h = handle_duplicate(object_kernel_handles(),
                                  (handle_t)a1, (uint32_t)a2);
    if (h == 0) return UINT64_MAX;
    return h;
}

static uint64_t handle_handle_get(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    object_t *obj = handle_get(object_kernel_handles(),
                               (handle_t)a1, (uint32_t)a2,
                               OBJECT_TYPE_NONE);
    if (obj == NULL) return UINT64_MAX;
    object_release(obj);
    return a1;
}

static uint64_t handle_object_lookup(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    object_t *obj = object_lookup(a1);
    if (obj == NULL) return UINT64_MAX;
    object_release(obj);
    return a1;
}

static uint64_t handle_object_release(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    object_release((object_t *)(uintptr_t)a1);
    return 0;
}

static uint64_t handle_object_count(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    return object_count();
}

static uint64_t handle_object_create(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    object_t *obj = object_create((object_type_t)a1, a2, NULL);
    if (obj == NULL) return UINT64_MAX;
    object_release(obj);
    return object_id(obj);
}

static uint64_t handle_object_retain(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    if (!object_retain((object_t *)(uintptr_t)a1)) {
        return UINT64_MAX;
    }
    return 0;
}

static uint64_t handle_read(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a4; (void)a5; (void)a6; (void)a7;
    ext4_fs_t *fs = ext4_get_current();
    if (fs == NULL) {
        kprintf("[syscall] read: ext4 not mounted\n");
        return -1;
    }
    /* a1 = file handle, a2 = user buffer, a3 = count */
    /* For now, simplified: read from the root file if handle is valid */
    uint8_t buf[4096];
    uint64_t nread = ext4_read(fs, 2, a2, buf, a3); /* simplified: always read from root inode */
    if (nread == 0) return 0;
    /* Copy kernel-space result back to user buffer (simplified) */
    return nread;
}

static uint64_t handle_write(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a4; (void)a5; (void)a6; (void)a7;
    ext4_fs_t *fs = ext4_get_current();
    if (fs == NULL) {
        kprintf("[syscall] write: ext4 not mounted\n");
        return -1;
    }
    /* a1 = file handle, a2 = user buffer, a3 = count */
    /* For now, simplified: write to root file */
    uint8_t buf[4096];
    /* Copy from user buffer - simplified, just attempt write */
    uint64_t nwritten = ext4_write(fs, 2, a2, buf, a3); /* simplified */
    return nwritten;
}

static uint64_t handle_read_input(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a1; (void)a2;
    /* kbd input via PS/2 - read from the keyboard buffer */
    (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    kprintf("[syscall] read_input: not yet implemented\n");
    return 0;
}

/* Lowest address above the user half: user pointers must stay below it. */
#define USER_VA_LIMIT 0x0000800000000000ULL

static uint64_t handle_debug_write(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    /* a1 = user buffer, a2 = length. Writes straight to the console; user
     * processes will get a console handle once handles reach them. */
    if (a2 > 256u || a1 >= USER_VA_LIMIT || a2 > USER_VA_LIMIT - a1) return UINT64_MAX;
    const char *p = (const char *)(uintptr_t)a1;
    for (uint64_t i = 0; i < a2; i++) console_putc(p[i]);
    return a2;
}

static uint64_t handle_exit(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    if (usermode_is_active()) {
        usermode_exit(a1);   /* unwinds to usermode_run_test; does not return */
    }
    kprintf("[syscall] exit: status=%llu\n", a1);
    thread_exit_current();
    return 0;
}

static uint64_t handle_brk(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    /* a1 = new program break value.
     * For now, just validate and return current break.
     * Real implementation would grow/shrink heap and switch program break. */
    uint64_t prev_brk = heap_brk();
    if (a1 == 0) return prev_brk;
    /* Simple: allow setting break to current + offset (if within heap) */
    if (a1 > prev_brk) {
        /* Try to extend heap - simplified */
        uint64_t result = (uint64_t)kmalloc(a1 - prev_brk);
        if (result == UINT64_MAX) return prev_brk;
        return a1;
    }
    return prev_brk;
}

/* ELF segment loading flags. */
#define ELF_PROT_READ  (1u << 0)
#define ELF_PROT_WRITE (1u << 1)
#define ELF_PROT_EXEC  (1u << 2)

/* Map a memory region into the current address space. */
static bool map_region(uint64_t virt, uint64_t phys, uint64_t flags) {
    bool ret = vmm_map(virt, phys, flags);
    (void)ret;  /* For now we don't check the return value */
    return ret;
}

/* Simple ELF loader that loads an ELF binary from the ramdisk filesystem.
 * For now, this is a basic implementation that reads the ELF file and
 * sets up basic mappings. The full ELF loading (including copying segment
 * data, setting up the user stack with argv/envp, etc.) is deferred.
 *
 * Returns 0 on success, or -errno on failure. */
static int elf_load_from_ramdisk(const char *filename, uint64_t *ret_rip) {
    (void)ret_rip;
    
    ext4_fs_t *fs = ext4_get_current();
    if (fs == NULL) {
        kprintf("[elf] ext4 not mounted\n");
        return -1;
    }
    
    /* Read the file from ramdisk. First, we need to find the inode.
     * The ramdisk root is inode 2. */
    uint64_t ino = 2;  /* root inode */
    
    /* Look up the file by name in the root directory. This is simplified -
     * we just read inode 2 directly as a stub. */
    uint8_t buf[4096];
    uint64_t nread = ext4_read(fs, ino, 0, buf, sizeof(buf));
    if (nread == 0) {
        kprintf("[elf] failed to read root inode\n");
        return -1;
    }
    
    /* Scan the directory entry for our filename. This is very simplified. */
    /* For now, just check if we can read the file at a known path. */
    kprintf("[elf] loading: %s from ramdisk\n", filename);
    
    /* Return the entry point at the start of the mapped region. */
    return 0;
}

/* Execute a new program. */
static uint64_t handle_execve(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    /* a1 = filename pointer (user-space virtual address) */
    
    /* Read the filename string from user space. We'll use it to look up
     * the file on the ramdisk filesystem. */
    char filename[256];
    size_t i;
    for (i = 0; i < sizeof(filename) - 1; i++) {
        char c;
        /* Copy user byte directly (simplified: assumes caller is kernel thread
         * or user address is mapped). Real implementation would copy from user
         * space via vmm_get_mapping. */
        c = (char)a1;
        filename[i] = c;
        if (c == '\0') break;
        a1++;
    }
    filename[i] = '\0';
    
    /* Look up the file on the ramdisk ext4 filesystem.
     * Inode 2 is the root directory. */
    ext4_fs_t *fs = ext4_get_current();
    if (fs == NULL) {
        kprintf("[syscall] execve: ext4 not mounted\n");
        return UINT64_MAX;
    }
    
    /* Read the root directory to find our file by name. */
    uint8_t dir_buf[4096];
    uint64_t nread = ext4_read(fs, 2, 0, dir_buf, sizeof(dir_buf));
    if (nread == 0) {
        kprintf("[syscall] execve: failed to read root inode\n");
        return UINT64_MAX;
    }
    
    /* Scan directory entries for our filename (simplified - reads first entry
     * as a placeholder; real implementation parses ext4 directory entries properly).
     * For now, just validate the file can be read. */
    kprintf("[syscall] execve: loading: %s\n", filename);
    
    uint8_t file_buf[4096];
    nread = ext4_read(fs, 2, 0, file_buf, sizeof(file_buf));
    if (nread == 0) {
        kprintf("[syscall] execve: failed to read file\n");
        return UINT64_MAX;
    }
    
    /* Parse ELF header - validate magic bytes */
    if (nread < 16 || file_buf[0] != 0x7f || file_buf[1] != 'E' ||
        file_buf[2] != 'L' || file_buf[3] != 'F') {
        kprintf("[syscall] execve: not an ELF file\n");
        return UINT64_MAX;
    }
    
    /* Get entry point from ELF header (offset 24, 8 bytes) */
    uint64_t entry_point = *(uint64_t *)(file_buf + 24);
    
    kprintf("[syscall] execve: entry point = 0x%llx\n",
            (unsigned long long)entry_point);
    return entry_point;
}

void syscall_dispatch(registers_t *regs) {
    uint64_t syscall_num = regs->rax;
    
    if (syscall_num >= 64u) {
        regs->rax = UINT64_MAX; /* invalid syscall number */
        return;
    }
    
    syscall_handler_t handler = syscall_table[syscall_num];
    if (handler == NULL) {
        regs->rax = UINT64_MAX; /* unimplmented syscall */
        return;
    }
    
    /* Call the handler with the 7-argument ABI signature. The user passed
     * arguments in rdi, rsi, rdx, r10, r8, r9; we forward them in that order.
     * The seventh argument is unused by all handlers. */
    regs->rax = handler(regs->rdi, regs->rsi, regs->rdx,
                        regs->r10, regs->r8, regs->r9, 0);
}