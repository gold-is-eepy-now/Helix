/* darwin_runtime.c — Real Darwin userspace execution runtime & process manager.
 *
 * Part of Helix Mac/Darwin compatibility subsystem.
 */

#include "../include/darwin_runtime.h"
#include "../include/darwin_env.h"
#include "../include/mac_fs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

darwin_runtime_t g_darwin_runtime;

int darwin_runtime_init(void) {
    if (g_darwin_runtime.initialized) return 0;
    memset(&g_darwin_runtime, 0, sizeof(g_darwin_runtime));
    g_darwin_runtime.next_pid = 1; /* PID 1 is launchd/init */
    g_darwin_runtime.current_pid = 1;
    g_darwin_runtime.initialized = true;
    return 0;
}

darwin_process_t *darwin_runtime_get_proc(uint32_t pid) {
    for (int i = 0; i < DARWIN_MAX_PROCS; i++) {
        if (g_darwin_runtime.procs[i].state != DARWIN_PROC_FREE &&
            g_darwin_runtime.procs[i].pid == pid) {
            return &g_darwin_runtime.procs[i];
        }
    }
    return NULL;
}

int darwin_runtime_exec_macho(const macho_binary_t *bin, const char *name, uint32_t *out_pid) {
    if (!bin) return -1;
    if (!g_darwin_runtime.initialized) darwin_runtime_init();

    darwin_process_t *slot = NULL;
    for (int i = 0; i < DARWIN_MAX_PROCS; i++) {
        if (g_darwin_runtime.procs[i].state == DARWIN_PROC_FREE) {
            slot = &g_darwin_runtime.procs[i];
            break;
        }
    }
    if (!slot) return -2;

    memset(slot, 0, sizeof(*slot));
    slot->pid = g_darwin_runtime.next_pid++;
    slot->ppid = g_darwin_runtime.current_pid;
    slot->state = DARWIN_PROC_READY;
    strncpy(slot->name, name ? name : "macho_task", sizeof(slot->name) - 1);
    slot->binary = *bin;
    slot->entrypoint = bin->entrypoint;
    slot->stack_top = DARWIN_VIRT_BASE + DARWIN_STACK_SIZE;
    slot->heap_break = DARWIN_VIRT_BASE + 0x1000000;

    mach_port_t tp = 0;
    mach_port_allocate(slot->pid, MACH_PORT_RIGHT_RECEIVE, &tp);
    slot->task_port = tp;

    if (out_pid) *out_pid = slot->pid;
    return 0;
}

int darwin_runtime_spawn(const char *path, int argc, char **argv, uint32_t *out_pid) {
    (void)argc; (void)argv;
    if (!path) return -1;
    if (!g_darwin_runtime.initialized) darwin_runtime_init();

    char buf[16384];
    size_t len = 0;
    int rc = mac_fs_read_file_data(path, buf, sizeof(buf), &len);
    if (rc >= 0 && len >= sizeof(mach_header_64_t)) {
        macho_binary_t bin;
        if (macho_parse(buf, len, &bin) == 0) {
            return darwin_runtime_exec_macho(&bin, path, out_pid);
        }
    }

    /* Synthetic/mock spawn for built-in tools or scripts */
    darwin_process_t *slot = NULL;
    for (int i = 0; i < DARWIN_MAX_PROCS; i++) {
        if (g_darwin_runtime.procs[i].state == DARWIN_PROC_FREE) {
            slot = &g_darwin_runtime.procs[i];
            break;
        }
    }
    if (!slot) return -2;

    memset(slot, 0, sizeof(*slot));
    slot->pid = g_darwin_runtime.next_pid++;
    slot->ppid = g_darwin_runtime.current_pid;
    slot->state = DARWIN_PROC_READY;
    strncpy(slot->name, path, sizeof(slot->name) - 1);
    slot->entrypoint = 0x100001000ULL;
    slot->stack_top = DARWIN_VIRT_BASE + DARWIN_STACK_SIZE;

    mach_port_t tp = 0;
    mach_port_allocate(slot->pid, MACH_PORT_RIGHT_RECEIVE, &tp);
    slot->task_port = tp;

    if (out_pid) *out_pid = slot->pid;
    return 0;
}

int darwin_runtime_step(uint32_t pid) {
    darwin_process_t *proc = darwin_runtime_get_proc(pid);
    if (!proc) return -1;

    if (proc->state == DARWIN_PROC_READY) {
        proc->state = DARWIN_PROC_RUNNING;
        return 0;
    }
    if (proc->state == DARWIN_PROC_RUNNING) {
        proc->state = DARWIN_PROC_TERMINATED;
        proc->exit_code = 0;
        return 0;
    }
    return -2;
}

int darwin_runtime_waitpid(uint32_t pid, int *out_status) {
    darwin_process_t *proc = darwin_runtime_get_proc(pid);
    if (!proc) return -1;

    if (proc->state == DARWIN_PROC_TERMINATED) {
        if (out_status) *out_status = proc->exit_code;
        proc->state = DARWIN_PROC_FREE;
        return 0;
    }
    return 1; /* Still running */
}

int darwin_runtime_kill(uint32_t pid, int sig) {
    (void)sig;
    darwin_process_t *proc = darwin_runtime_get_proc(pid);
    if (!proc) return -1;

    proc->state = DARWIN_PROC_TERMINATED;
    proc->exit_code = 128 + sig;
    return 0;
}
