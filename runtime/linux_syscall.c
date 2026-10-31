/* linux_syscall.c — Linux/BSD syscall translation for Helix OS.
 *
 * This file implements the system call translation layer that maps Linux
 * system calls (via the x86_64 ABI) into Helix native syscalls.
 *
 * Translation table: each Linux syscall number is mapped to a native Helix
 * syscall index and (where needed) a dedicated translation function that
 * adjusts arguments and return values for the ABI differences.
 */

#include "linux_syscall.h"
#include "linux_runtime.h"
#include "elf_loader.h"
#include <stddef.h>

/* ---------------------------------------------------------------------------
 * Native syscall invocation via int $0x80
 *
 * Uses inline assembly to call kernel syscalls using the Linux ABI style
 * int $0x80 instruction. The native syscall numbers are different from Linux
 * ones, so we need to set rax appropriately.
 * -------------------------------------------------------------------------- */

/* Native syscall invocation using int $0x80 */
static inline uint64_t invoke_native_syscall(int native_num, uint64_t arg1,
                                              uint64_t arg2, uint64_t arg3,
                                              uint64_t arg4, uint64_t arg5,
                                              uint64_t arg6, uint64_t arg7) {
    uint64_t result;
    
    /* Set up arguments in registers as expected by kernel (x86-64 syscall ABI):
     * rax = syscall number, rdi = arg1, rsi = arg2, rdx = arg3,
     * r10 = arg4, r8 = arg5, r9 = arg6
     * int $0x80 invokes the kernel syscall handler directly. */
    __asm__ volatile (
        "movq %1, %%rax\n"
        "movq %2, %%rdi\n"
        "movq %3, %%rsi\n"
        "movq %4, %%rdx\n"
        "movq %5, %%r10\n"
        "movq %6, %%r8\n"
        "movq %7, %%r9\n"
        "int $0x80\n"
        "movq %%rax, %0\n"
        : "=r"(result)
        : "r"((uint64_t)native_num), "r"(arg1), "r"(arg2), "r"(arg3),
          "r"(arg4), "r"(arg5), "r"(arg6), "r"(arg7)
        : "rax", "rdi", "rsi", "rdx", "r10", "r8", "r9"
    );
    
    return result;
}

/* Public wrapper for native syscall invocation. */
uint64_t native_syscall_invoke(int native_num, uint64_t arg1, uint64_t arg2,
                                uint64_t arg3, uint64_t arg4,
                                uint64_t arg5, uint64_t arg6, uint64_t arg7) {
    return invoke_native_syscall(native_num, arg1, arg2, arg3, arg4, arg5, arg6, arg7);
}

/* ---------------------------------------------------------------------------
 * Translation table — all known Linux x86_64 system calls
 *
 * Direct-indexed by Linux syscall number. Linux x86_64 currently assigns
 * numbers 0..466 (arch/x86/entry/syscalls/syscall_64.tbl); 512 slots cover
 * every known call with headroom. Slots default to -1, so any call the
 * kernel cannot serve becomes ENOSYS rather than hitting an unrelated
 * native syscall.
 *
 * Mapped entries are the closest native equivalent. read/write/open/
 * close/dup/exit/brk match argument-for-argument; mmap/clone/execve land on
 * native stubs whose argument layout still needs kernel-side agreement.
 *
 * ponytail: openat(257) and friends pass dirfd as arg1 (path is arg2);
 * mapping them needs a per-syscall argument shifter. Add one here when a
 * real Linux binary needs openat/statx — until then they return ENOSYS.
 * ------------------------------------------------------------------------- */

#define LINUX_NR_MAX 512   /* covers all known x86_64 Linux syscall numbers */

_Static_assert(SYSCALL_NATIVE_MAX <= 64, "kernel syscall_table has 64 slots");

typedef struct {
    int16_t linux_nr;
    int16_t native;        /* native Helix syscall index */
    uint8_t fd_arg1;       /* arg1 is a Linux fd needing handle translation */
} linux_map_entry_t;

static const linux_map_entry_t k_linux_map[] = {
    /* memory */
    {  9, SYSCALL_MAP,           0},  /* mmap */
    { 11, SYSCALL_UNMAP,         0},  /* munmap */
    { 12, SYSCALL_BRK,           0},  /* brk */
    /* files and descriptors */
    {  0, SYSCALL_READ,          1},  /* read */
    {  1, SYSCALL_WRITE,         1},  /* write */
    {  2, SYSCALL_HANDLE_CREATE, 0},  /* open */
    {  3, SYSCALL_HANDLE_CLOSE,  1},  /* close */
    { 32, SYSCALL_HANDLE_DUPLICATE, 1},  /* dup */
    { 33, SYSCALL_HANDLE_DUPLICATE, 1},  /* dup2 */
    { 85, SYSCALL_HANDLE_CREATE, 0},  /* creat */
    {292, SYSCALL_HANDLE_DUPLICATE, 1},  /* dup3 */
    /* processes and threads */
    { 24, SYSCALL_THREAD_YIELD,  0},  /* sched_yield */
    { 56, SYSCALL_THREAD_CREATE, 0},  /* clone */
    { 59, SYSCALL_EXECVE,        0},  /* execve */
    { 60, SYSCALL_EXIT,          0},  /* exit */
    {231, SYSCALL_EXIT,          0},  /* exit_group */
};

static int16_t g_linux_to_native[LINUX_NR_MAX];
static uint8_t g_linux_fd_arg1[LINUX_NR_MAX];
static int g_syscall_map_initialized = 0;

static void syscall_table_init(void) {
    int i;
    if (g_syscall_map_initialized) return;

    for (i = 0; i < LINUX_NR_MAX; i++) {
        g_linux_to_native[i] = -1;   /* unknown -> ENOSYS */
        g_linux_fd_arg1[i] = 0;
    }
    for (i = 0; i < (int)(sizeof(k_linux_map) / sizeof(k_linux_map[0])); i++) {
        g_linux_to_native[k_linux_map[i].linux_nr] = k_linux_map[i].native;
        g_linux_fd_arg1[k_linux_map[i].linux_nr] = k_linux_map[i].fd_arg1;
    }
    g_syscall_map_initialized = 1;
}

/* ---------------------------------------------------------------------------
 * Public interface
 * ------------------------------------------------------------------------- */

/* Map a Linux syscall number to its native Helix equivalent.
 * Returns the native syscall index, or -1 if unmapped. Every known Linux
 * x86_64 number is accepted; ones without a native equivalent return -1. */
int map_linux_to_native(int linux_num) {
    if (!g_syscall_map_initialized) syscall_table_init();
    if (linux_num < 0 || linux_num >= LINUX_NR_MAX) return -1;
    return g_linux_to_native[linux_num];
}

/* translate_linux_syscall — translate a Linux syscall to Helix native.
 *
 * Intercepts Linux syscalls from a running binary and translates them
 * into Helix native calls. For fd-based syscalls, arg1 (the Linux fd)
 * is mapped to a Helix handle first.
 *
 * Returns the result of the translated native call.
 */
uint64_t linux_translate_syscall(int number, uint64_t arg1, uint64_t arg2,
                                 uint64_t arg3, uint64_t arg4,
                                 uint64_t arg5, uint64_t arg6, uint64_t arg7) {
    int native_num;
    (void)arg4; (void)arg5; (void)arg6; (void)arg7;

    native_num = map_linux_to_native(number);
    if (native_num < 0) {
        /* Unmapped syscall — return ENOSYS (-38 on Linux) */
        return (uint64_t)-38;
    }

    /* fd-based syscalls: arg1 is a Linux fd, map it to a Helix handle */
    if (number >= 0 && number < LINUX_NR_MAX && g_linux_fd_arg1[number]) {
        handle_t handle = linux_fd_to_handle((int)arg1);
        if (handle == 0) {
            return (uint64_t)-9;  /* EBADF */
        }
        arg1 = (uint64_t)handle;
    }

    /* Invoke native Helix syscall via int $0x80 */
    return invoke_native_syscall(native_num, arg1, arg2, arg3, arg4, arg5, arg6, arg7);
}

/* Get the current error number (errno) for the calling Linux process.
 * errno values follow Linux conventions (e.g., ENOSYS = 38, EBADF = 9). */
int linux_errno(void) {
    /* The last syscall was translated and returned an error code.
     * In a real implementation, this would be tracked per-thread. */
    return 38;  /* ENOSYS */
}