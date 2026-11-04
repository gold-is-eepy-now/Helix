/* darwin_runtime.h — Real Darwin userspace execution runtime & process manager.
 *
 * Part of Helix Mac/Darwin compatibility subsystem.
 */

#ifndef MAC_DARWIN_RUNTIME_H
#define MAC_DARWIN_RUNTIME_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "mach_o.h"
#include "darwin_syscall.h"
#include "mach_port.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DARWIN_MAX_PROCS         16
#define DARWIN_STACK_SIZE        (8 * 1024 * 1024) /* 8MB default stack */
#define DARWIN_VIRT_BASE         0x100000000ULL

typedef enum {
    DARWIN_PROC_FREE = 0,
    DARWIN_PROC_READY,
    DARWIN_PROC_RUNNING,
    DARWIN_PROC_BLOCKED,
    DARWIN_PROC_TERMINATED
} darwin_proc_state_t;

typedef struct {
    uint32_t            pid;
    uint32_t            ppid;
    darwin_proc_state_t state;
    char                name[64];
    uint64_t            entrypoint;
    uint64_t            stack_top;
    uint64_t            heap_break;
    uint32_t            task_port;
    int                 exit_code;
    macho_binary_t      binary;
} darwin_process_t;

typedef struct {
    darwin_process_t procs[DARWIN_MAX_PROCS];
    uint32_t         next_pid;
    uint32_t         current_pid;
    bool             initialized;
} darwin_runtime_t;

extern darwin_runtime_t g_darwin_runtime;

/* Runtime lifecycle APIs */
int  darwin_runtime_init(void);
int  darwin_runtime_spawn(const char *path, int argc, char **argv, uint32_t *out_pid);
int  darwin_runtime_exec_macho(const macho_binary_t *bin, const char *name, uint32_t *out_pid);
int  darwin_runtime_step(uint32_t pid);
int  darwin_runtime_waitpid(uint32_t pid, int *out_status);
int  darwin_runtime_kill(uint32_t pid, int sig);
darwin_process_t *darwin_runtime_get_proc(uint32_t pid);

#ifdef __cplusplus
}
#endif

#endif /* MAC_DARWIN_RUNTIME_H */
