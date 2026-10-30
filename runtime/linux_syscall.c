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

/* ---------------------------------------------------------------------------
 * Translation table
 *
 * Maps Linux syscall numbers to native Helix syscall indices. The table is
 * populated at first use (lazy initialization).
 * ------------------------------------------------------------------------- */

#define LINUX_SYSCALL_MAX 64

typedef struct {
    int linux_num;
    int native_index;       /* Index into kernel syscall_table, -1 = unmapped */
    int wants_fd_translate; /* True if arg1 is a Linux fd needing handle mapping */
} syscall_map_entry_t;

/* Translation table: Linux syscall number -> native syscall index */
static syscall_map_entry_t g_syscall_map[LINUX_SYSCALL_MAX];
static int g_syscall_map_initialized = 0;

/* Populate the syscall translation table with known Linux->Helix mappings. */
static void syscall_table_init(void) {
    int i;
    if (g_syscall_map_initialized) return;

    for (i = 0; i < LINUX_SYSCALL_MAX; i++) {
        g_syscall_map[i].linux_num = -1;
        g_syscall_map[i].native_index = -1;
        g_syscall_map[i].wants_fd_translate = 0;
    }

    /* Linux read(0, buf, count) -> Helix read */
    for (i = 0; i < LINUX_SYSCALL_MAX; i++) {
        if (g_syscall_map[i].linux_num == -1) {
            g_syscall_map[i].linux_num = 0;
            g_syscall_map[i].native_index = SYSCALL_READ;
            g_syscall_map[i].wants_fd_translate = 1;
            break;
        }
    }

    /* Linux write(1, buf, count) -> Helix write */
    for (i = 0; i < LINUX_SYSCALL_MAX; i++) {
        if (g_syscall_map[i].linux_num == -1) {
            g_syscall_map[i].linux_num = 1;
            g_syscall_map[i].native_index = SYSCALL_WRITE;
            g_syscall_map[i].wants_fd_translate = 1;
            break;
        }
    }

    /* Linux exit(status) -> Helix exit */
    for (i = 0; i < LINUX_SYSCALL_MAX; i++) {
        if (g_syscall_map[i].linux_num == -1) {
            g_syscall_map[i].linux_num = 60;
            g_syscall_map[i].native_index = SYSCALL_EXIT;
            g_syscall_map[i].wants_fd_translate = 0;
            break;
        }
    }

    /* Linux brk(addr) -> Helix brk */
    for (i = 0; i < LINUX_SYSCALL_MAX; i++) {
        if (g_syscall_map[i].linux_num == -1) {
            g_syscall_map[i].linux_num = 12;
            g_syscall_map[i].native_index = SYSCALL_BRK;
            g_syscall_map[i].wants_fd_translate = 0;
            break;
        }
    }

    /* Linux open(filename, flags, mode) -> Helix handle_create */
    for (i = 0; i < LINUX_SYSCALL_MAX; i++) {
        if (g_syscall_map[i].linux_num == -1) {
            g_syscall_map[i].linux_num = 2;
            g_syscall_map[i].native_index = SYSCALL_HANDLE_CREATE;
            g_syscall_map[i].wants_fd_translate = 0;
            break;
        }
    }

    /* Linux close(fd) -> Helix handle_close */
    for (i = 0; i < LINUX_SYSCALL_MAX; i++) {
        if (g_syscall_map[i].linux_num == -1) {
            g_syscall_map[i].linux_num = 3;
            g_syscall_map[i].native_index = SYSCALL_HANDLE_CLOSE;
            g_syscall_map[i].wants_fd_translate = 1;
            break;
        }
    }

    g_syscall_map_initialized = 1;
}

/* ---------------------------------------------------------------------------
 * Public interface
 * ------------------------------------------------------------------------- */

/* Map a Linux syscall number to its native Helix equivalent.
 * Returns the native syscall index, or -1 if unmapped. */
int map_linux_to_native(int linux_num) {
    if (!g_syscall_map_initialized) syscall_table_init();

    int i;
    for (i = 0; i < LINUX_SYSCALL_MAX; i++) {
        if (g_syscall_map[i].linux_num == linux_num) {
            return g_syscall_map[i].native_index;
        }
    }
    return -1;
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

    /* For fd-based syscalls, translate Linux fd to Helix handle */
    if (number == 0 || number == 1 || number == 3) {
        /* read/write/close: arg1 is the Linux fd, translate it */
        handle_t handle = linux_fd_to_handle((int)arg1);
        if (handle == 0) {
            return (uint64_t)-9;  /* EBADF */
        }
        /* Replace arg1 with the Helix handle */
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