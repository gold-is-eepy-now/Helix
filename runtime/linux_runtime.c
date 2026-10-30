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

/* --- Linux syscall dispatch table ---

 * Each entry maps a Linux syscall number to:
 * - The native Helix syscall index
 * - A function that translates arguments from Linux format to Helix format
 * - A function that translates the return value
 *
 * The dispatch uses the kernel's native syscall table placed at .syscall_table.
 */

#define LINUX_SYSCALL_MAX 64

typedef struct {
    int linux_num;
    int native_index;   /* Index into kernel syscall_table, or negative for special */
    int has_arg_translate;
} linux_native_map_t;

static linux_native_map_t g_syscall_map[LINUX_SYSCALL_MAX];

static void syscall_map_init(void) {
    int i;
    /* Initialize all entries as unmapped */
    for (i = 0; i < LINUX_SYSCALL_MAX; i++) {
        g_syscall_map[i].linux_num = -1;
        g_syscall_map[i].native_index = -1;
        g_syscall_map[i].has_arg_translate = 0;
    }

    /* Map common Linux syscalls to native Helix equivalents */
    /* Linux read(0, buf, count) -> Helix read(handle, buf, count) */
    g_syscall_map[0].linux_num = 0;
    g_syscall_map[0].native_index = SYSCALL_READ;  /* Native read syscall */
    g_syscall_map[0].has_arg_translate = 1;

    /* Linux write(1, buf, count) -> Helix write(handle, buf, count) */
    g_syscall_map[1].linux_num = 1;
    g_syscall_map[1].native_index = SYSCALL_WRITE;
    g_syscall_map[1].has_arg_translate = 1;

    /* Linux exit(status) -> Helix exit(status) */
    g_syscall_map[60].linux_num = 60;  /* Linux exit */
    g_syscall_map[60].native_index = SYSCALL_EXIT;
    g_syscall_map[60].has_arg_translate = 0;

    /* Linux brk(addr) -> Helix brk(addr) */
    g_syscall_map[12].linux_num = 12;  /* Linux brk */
    g_syscall_map[12].native_index = SYSCALL_BRK;
    g_syscall_map[12].has_arg_translate = 0;
}

/* --- Syscall translation ---

 * Translate a Linux syscall number and arguments to native Helix format.
 * Returns the native syscall index, or -1 if not mapped.
 */

int linux_translate_number(int linux_num) {
    /* Check if the syscall map is initialized by checking the first entry's linux_num. */
    if (g_syscall_map[0].linux_num == -1) syscall_map_init();
    
    int i;
    for (i = 0; i < LINUX_SYSCALL_MAX; i++) {
        if (g_syscall_map[i].linux_num == linux_num) {
            return g_syscall_map[i].native_index;
        }
    }
    return -1;  /* Unmapped syscall */
}