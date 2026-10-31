/* bsd_syscall.c — BSD system call translation for Helix OS.
 *
 * This file implements the system call translation layer that maps BSD system calls
 * (via the x86_64 ABI) into Helix native syscalls. BSD uses different syscall
 * numbers than Linux, so this layer provides the mapping.
 *
 * The BSD syscalls are translated to the same native Helix syscalls as Linux,
 * since both Linux and BSD use the same kernel interface (int $0x80 on x86_64).
 */

#include "bsd_syscall.h"
#include "linux_syscall.h"
#include "linux_runtime.h"
#include <stddef.h>

/* BSD syscall number mappings to native Helix syscall indices */
typedef struct {
    int bsd_num;
    int native_index;       /* Index into kernel syscall_table, -1 = unmapped */
} bsd_syscall_map_t;

/* BSD syscall number → native syscall index mapping */
static bsd_syscall_map_t g_bsd_syscall_map[256] = {0};  /* 256 covers all BSD syscalls */
static int g_bsd_syscall_map_initialized = 0;

/* Initialize the BSD syscall translation table */
static void bsd_syscall_table_init(void) {
    if (g_bsd_syscall_map_initialized) return;

    /* Map BSD syscalls to native Helix equivalents */
    /* BSD read(3, buf, count) → Helix read(handle, buf, count) */
    g_bsd_syscall_map[3].bsd_num = 3;
    g_bsd_syscall_map[3].native_index = SYSCALL_READ;
    
    /* BSD write(4, buf, count) → Helix write(handle, buf, count) */
    g_bsd_syscall_map[4].bsd_num = 4;
    g_bsd_syscall_map[4].native_index = SYSCALL_WRITE;
    
    /* BSD open(5, flags, mode) → Helix handle_create */
    g_bsd_syscall_map[5].bsd_num = 5;
    g_bsd_syscall_map[5].native_index = SYSCALL_HANDLE_CREATE;
    
    /* BSD close(6, fd) → Helix handle_close */
    g_bsd_syscall_map[6].bsd_num = 6;
    g_bsd_syscall_map[6].native_index = SYSCALL_HANDLE_CLOSE;
    
    /* BSD exit(1, status) → Helix exit(status) */
    g_bsd_syscall_map[1].bsd_num = 1;
    g_bsd_syscall_map[1].native_index = SYSCALL_EXIT;
    
    /* BSD brk is deprecated (use mmap), but map it to brk for compatibility */
    g_bsd_syscall_map[189].bsd_num = 189;
    g_bsd_syscall_map[189].native_index = SYSCALL_BRK;
    
    g_bsd_syscall_map_initialized = 1;
}

/* Translate a BSD syscall number to its native Helix equivalent.
 * Returns the native syscall index, or -1 if unmapped. */
int bsd_map_to_native(int bsd_num) {
    if (!g_bsd_syscall_map_initialized) bsd_syscall_table_init();

    for (int i = 0; i < 256; i++) {
        if (g_bsd_syscall_map[i].bsd_num == bsd_num) {
            return g_bsd_syscall_map[i].native_index;
        }
    }
    return -1;  /* Unmapped syscall */
}

/* Translate a BSD syscall to Helix native.
 * Returns the result of the translated native call. */
uint64_t bsd_translate_syscall(int number, uint64_t arg1, uint64_t arg2,
                                uint64_t arg3, uint64_t arg4,
                                uint64_t arg5, uint64_t arg6, uint64_t arg7) {
    int native_num = bsd_map_to_native(number);
    if (native_num < 0) {
        /* Unmapped syscall — return ENOSYS (-38 on Linux) */
        return (uint64_t)-38;
    }

    /* For fd-based syscalls, translate BSD fd to Helix handle */
    if (number == 3 || number == 4 || number == 5 || number == 6) {
        /* read/write/open/close: arg1 is the BSD fd, arg2 is the buffer */
        handle_t handle = linux_fd_to_handle((int)arg1);
        if (handle == 0) {
            return (uint64_t)-9;  /* EBADF */
        }
        /* Replace arg1 with the Helix handle */
        arg1 = (uint64_t)handle;
    }

    /* Invoke native Helix syscall directly (native_num is already translated) */
    return native_syscall_invoke(native_num, arg1, arg2, arg3, arg4, arg5, arg6, arg7);
}

/* Get the current error number (errno) for the calling BSD process. */
int bsd_errno(void) {
    /* In a real implementation, this would track per-thread error state.
     * For now, return a generic error code. */
    return 38;  /* ENOSYS */
}

/* Map BSD fd to Helix handle (delegates to Linux runtime FD table) */
handle_t bsd_fd_to_handle(int fd) {
    return linux_fd_to_handle(fd);
}

/* Set BSD fd to Helix handle (delegates to Linux runtime FD table) */
int bsd_set_fd_handle(int fd, handle_t handle) {
    return linux_set_fd_handle(fd, handle);
}