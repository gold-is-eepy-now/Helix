/* linux_runtime.c — Linux/BSD runtime for Helix OS.
 *
 * Implements a userspace Linux compatibility layer that:
 * 1. Loads Linux/BSD ELF binaries
 * 2. Translates Linux system calls to Helix native syscalls
 * 3. Maps Linux file descriptors to Helix handles
 *
 * This code runs as a userspace process and uses the kernel's native
 * syscall interface (not kernel internals) for translation.
 *
 * Freestanding implementation - no host libc.
 */

#include "linux_runtime.h"
#include "linux_syscall.h"
#include "elf_loader.h"
#include <stddef.h>

/* --- Process state --- */

#define MAX_LINUX_FDS 256

typedef struct {
    int initialized;
    handle_t fd_table[MAX_LINUX_FDS];  /* Linux fd -> Helix handle mapping */
} linux_process_state_t;

static linux_process_state_t g_linux_state;

/* --- FD mapping --- */

void linux_fd_table_init(void) {
    int i;
    if (g_linux_state.initialized) return;
    for (i = 0; i < MAX_LINUX_FDS; i++) {
        g_linux_state.fd_table[i] = 0;
    }
    g_linux_state.initialized = 1;
}

handle_t linux_fd_to_handle(int fd) {
    if (fd < 0 || fd >= MAX_LINUX_FDS) return 0;
    if (!g_linux_state.initialized) linux_fd_table_init();
    return g_linux_state.fd_table[fd];
}

int linux_set_fd_handle(int fd, handle_t handle) {
    if (fd < 0 || fd >= MAX_LINUX_FDS) return -1;
    if (!g_linux_state.initialized) linux_fd_table_init();
    g_linux_state.fd_table[fd] = handle;
    return 0;
}

/* --- Syscall translation ---

 * Thin wrapper over the master table in linux_syscall.c. Single source of
 * truth: every known Linux x86_64 number maps there (or -1 = ENOSYS).
 */

int linux_translate_number(int linux_num) {
    return map_linux_to_native(linux_num);
}