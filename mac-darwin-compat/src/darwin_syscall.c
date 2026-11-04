/* darwin_syscall.c — Darwin BSD and Mach syscall / trap translation.
 *
 * Part of Helix Mac/Darwin compatibility subsystem.
 */

#include "../include/darwin_syscall.h"
#include "../include/mach_port.h"
#include "../include/mac_fs.h"
#include "../include/darwin_env.h"
#include <string.h>

/* Native Helix syscall constants (pinned to match kernel/src/syscall.h) */
#define HELIX_SYSCALL_MAP             4
#define HELIX_SYSCALL_UNMAP           5
#define HELIX_SYSCALL_THREAD_CREATE   6
#define HELIX_SYSCALL_THREAD_YIELD    8
#define HELIX_SYSCALL_HANDLE_CREATE   11
#define HELIX_SYSCALL_HANDLE_CLOSE    12
#define HELIX_SYSCALL_HANDLE_DUPLICATE 13
#define HELIX_SYSCALL_READ            20
#define HELIX_SYSCALL_WRITE           21
#define HELIX_SYSCALL_EXIT            23
#define HELIX_SYSCALL_BRK             24
#define HELIX_SYSCALL_EXECVE          25

/* Freestanding native syscall invocation declaration (defined in runtime or mock) */
__attribute__((weak))
uint64_t native_syscall_invoke(int native_num, uint64_t arg1, uint64_t arg2,
                                uint64_t arg3, uint64_t arg4, uint64_t arg5,
                                uint64_t arg6, uint64_t arg7) {
    (void)native_num; (void)arg1; (void)arg2; (void)arg3;
    (void)arg4; (void)arg5; (void)arg6; (void)arg7;
    return 0;
}

int64_t darwin_translate_number(uint64_t raw_num, bool *out_is_mach) {
    if (out_is_mach) *out_is_mach = false;

    /* Negative numbers are classic Mach traps: e.g. -26 is mach_reply_port */
    if ((int64_t)raw_num < 0) {
        if (out_is_mach) *out_is_mach = true;
        return (int64_t)(-(int64_t)raw_num);
    }

    uint32_t cls = (uint32_t)(raw_num & DARWIN_CLASS_MASK);
    uint32_t num = (uint32_t)(raw_num & DARWIN_SYSCALL_MASK);

    if (cls == DARWIN_CLASS_MACH) {
        if (out_is_mach) *out_is_mach = true;
        return (int64_t)num;
    }

    if (cls == DARWIN_CLASS_BSD || cls == DARWIN_CLASS_NONE) {
        /* BSD syscall number */
        switch (num) {
            case DARWIN_SYS_exit:            return HELIX_SYSCALL_EXIT;
            case DARWIN_SYS_read:            return HELIX_SYSCALL_READ;
            case DARWIN_SYS_write:           return HELIX_SYSCALL_WRITE;
            case DARWIN_SYS_open:            return HELIX_SYSCALL_HANDLE_CREATE;
            case DARWIN_SYS_close:           return HELIX_SYSCALL_HANDLE_CLOSE;
            case DARWIN_SYS_dup:
            case DARWIN_SYS_dup2:            return HELIX_SYSCALL_HANDLE_DUPLICATE;
            case DARWIN_SYS_mmap:            return HELIX_SYSCALL_MAP;
            case DARWIN_SYS_munmap:          return HELIX_SYSCALL_UNMAP;
            case DARWIN_SYS_sched_yield:     return HELIX_SYSCALL_THREAD_YIELD;
            case DARWIN_SYS_fork:
            case DARWIN_SYS_bsdthread_create: return HELIX_SYSCALL_THREAD_CREATE;
            case DARWIN_SYS_execve:          return HELIX_SYSCALL_EXECVE;
            default:                         return -1;
        }
    }

    return -1;
}

/* Sysctl handler for macOS identifiers and hardware queries */
int darwin_sysctl_query(const int *name, uint32_t namelen, void *oldp,
                        size_t *oldlenp, const void *newp, size_t newlen) {
    (void)newp; (void)newlen;
    if (!name || namelen < 2 || !oldp || !oldlenp) {
        return -DARWIN_EINVAL;
    }

    int top = name[0];
    int sub = name[1];

    if (top == CTL_KERN) {
        switch (sub) {
            case KERN_OSTYPE: {
                const char *ostype = "Darwin";
                size_t len = strlen(ostype) + 1;
                if (*oldlenp < len) len = *oldlenp;
                memcpy(oldp, ostype, len);
                *oldlenp = len;
                return 0;
            }
            case KERN_OSRELEASE: {
                const char *rel = "23.5.0";
                size_t len = strlen(rel) + 1;
                if (*oldlenp < len) len = *oldlenp;
                memcpy(oldp, rel, len);
                *oldlenp = len;
                return 0;
            }
            case KERN_VERSION: {
                const char *ver = "Darwin Kernel Version 23.5.0: Helix Mac-Darwin Subsystem (x86_64)";
                size_t len = strlen(ver) + 1;
                if (*oldlenp < len) len = *oldlenp;
                memcpy(oldp, ver, len);
                *oldlenp = len;
                return 0;
            }
            case KERN_HOSTNAME: {
                const char *host = "Helix-Mac.local";
                size_t len = strlen(host) + 1;
                if (*oldlenp < len) len = *oldlenp;
                memcpy(oldp, host, len);
                *oldlenp = len;
                return 0;
            }
            default:
                break;
        }
    } else if (top == CTL_HW) {
        switch (sub) {
            case HW_MACHINE: {
                const char *mach = "x86_64";
                size_t len = strlen(mach) + 1;
                if (*oldlenp < len) len = *oldlenp;
                memcpy(oldp, mach, len);
                *oldlenp = len;
                return 0;
            }
            case HW_MODEL: {
                const char *model = "Macmini8,1";
                size_t len = strlen(model) + 1;
                if (*oldlenp < len) len = *oldlenp;
                memcpy(oldp, model, len);
                *oldlenp = len;
                return 0;
            }
            case HW_NCPU: {
                uint32_t ncpu = 4;
                if (*oldlenp >= sizeof(uint32_t)) {
                    memcpy(oldp, &ncpu, sizeof(uint32_t));
                    *oldlenp = sizeof(uint32_t);
                    return 0;
                }
                return -DARWIN_EINVAL;
            }
            case HW_PAGESIZE: {
                uint32_t pagesize = 4096;
                if (*oldlenp >= sizeof(uint32_t)) {
                    memcpy(oldp, &pagesize, sizeof(uint32_t));
                    *oldlenp = sizeof(uint32_t);
                    return 0;
                }
                return -DARWIN_EINVAL;
            }
            case HW_MEMSIZE: {
                uint64_t memsize = 16ULL * 1024 * 1024 * 1024; /* 16 GiB */
                if (*oldlenp >= sizeof(uint64_t)) {
                    memcpy(oldp, &memsize, sizeof(uint64_t));
                    *oldlenp = sizeof(uint64_t);
                    return 0;
                }
                return -DARWIN_EINVAL;
            }
            default:
                break;
        }
    }

    return -DARWIN_ENOENT;
}

int darwin_sysctlbyname(const char *name, void *oldp, size_t *oldlenp,
                        const void *newp, size_t newlen) {
    if (!name) return -DARWIN_EINVAL;

    if (strcmp(name, "kern.ostype") == 0) {
        int mib[2] = { CTL_KERN, KERN_OSTYPE };
        return darwin_sysctl_query(mib, 2, oldp, oldlenp, newp, newlen);
    }
    if (strcmp(name, "kern.osrelease") == 0) {
        int mib[2] = { CTL_KERN, KERN_OSRELEASE };
        return darwin_sysctl_query(mib, 2, oldp, oldlenp, newp, newlen);
    }
    if (strcmp(name, "kern.version") == 0) {
        int mib[2] = { CTL_KERN, KERN_VERSION };
        return darwin_sysctl_query(mib, 2, oldp, oldlenp, newp, newlen);
    }
    if (strcmp(name, "kern.hostname") == 0) {
        int mib[2] = { CTL_KERN, KERN_HOSTNAME };
        return darwin_sysctl_query(mib, 2, oldp, oldlenp, newp, newlen);
    }
    if (strcmp(name, "hw.machine") == 0) {
        int mib[2] = { CTL_HW, HW_MACHINE };
        return darwin_sysctl_query(mib, 2, oldp, oldlenp, newp, newlen);
    }
    if (strcmp(name, "hw.model") == 0) {
        int mib[2] = { CTL_HW, HW_MODEL };
        return darwin_sysctl_query(mib, 2, oldp, oldlenp, newp, newlen);
    }
    if (strcmp(name, "hw.ncpu") == 0) {
        int mib[2] = { CTL_HW, HW_NCPU };
        return darwin_sysctl_query(mib, 2, oldp, oldlenp, newp, newlen);
    }
    if (strcmp(name, "hw.pagesize") == 0) {
        int mib[2] = { CTL_HW, HW_PAGESIZE };
        return darwin_sysctl_query(mib, 2, oldp, oldlenp, newp, newlen);
    }
    if (strcmp(name, "hw.memsize") == 0) {
        int mib[2] = { CTL_HW, HW_MEMSIZE };
        return darwin_sysctl_query(mib, 2, oldp, oldlenp, newp, newlen);
    }

    return -DARWIN_ENOENT;
}

int64_t darwin_syscall_dispatch(uint64_t raw_num, uint64_t a1, uint64_t a2,
                                uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    bool is_mach = false;
    int64_t translated = darwin_translate_number(raw_num, &is_mach);

    if (is_mach) {
        switch (translated) {
            case MACH_TRAP_reply_port:
                return (int64_t)mach_reply_port();
            case MACH_TRAP_task_self:
                return (int64_t)mach_task_self();
            case MACH_TRAP_host_self:
                return (int64_t)mach_host_self();
            case MACH_TRAP_port_allocate:
                return (int64_t)mach_port_allocate((mach_port_t)a1, (mach_port_right_t)a2, (mach_port_t *)a3);
            case MACH_TRAP_port_deallocate:
                return (int64_t)mach_port_deallocate((mach_port_t)a1, (mach_port_t)a2);
            case MACH_TRAP_port_insert_right:
                return (int64_t)mach_port_insert_right((mach_port_t)a1, (mach_port_t)a2, (mach_port_t)a3, (mach_msg_bits_t)a4);
            case MACH_TRAP_msg:
                return (int64_t)mach_msg_trap_impl((mach_msg_header_t *)a1, (uint32_t)a2,
                                                   (mach_msg_size_t)a3, (mach_msg_size_t)a4,
                                                   (mach_port_t)a5, (mach_msg_timeout_t)a6, 0);
            default:
                return -DARWIN_ENOSYS;
        }
    }

    uint32_t bsd_num = (uint32_t)(raw_num & DARWIN_SYSCALL_MASK);

    switch (bsd_num) {
        case DARWIN_SYS_exit:
            return (int64_t)native_syscall_invoke(HELIX_SYSCALL_EXIT, a1, 0, 0, 0, 0, 0, 0);

        case DARWIN_SYS_read:
            return mac_fs_read((int)a1, (void *)a2, (size_t)a3);

        case DARWIN_SYS_write:
            return mac_fs_write((int)a1, (const void *)a2, (size_t)a3);

        case DARWIN_SYS_open:
            return mac_fs_open((const char *)a1, (int)a2, (uint32_t)a3);

        case DARWIN_SYS_close:
            return mac_fs_close((int)a1);

        case DARWIN_SYS_lseek:
            return mac_fs_lseek((int)a1, (int64_t)a2, (int)a3);

        case DARWIN_SYS_getpid:
            return (int64_t)g_darwin_env.pid;

        case DARWIN_SYS_getppid:
            return (int64_t)g_darwin_env.ppid;

        case DARWIN_SYS_getuid:
        case DARWIN_SYS_geteuid:
            return (int64_t)g_darwin_env.uid;

        case DARWIN_SYS_getgid:
        case DARWIN_SYS_getegid:
            return (int64_t)g_darwin_env.gid;

        case DARWIN_SYS___sysctl:
            return darwin_sysctl_query((const int *)a1, (uint32_t)a2, (void *)a3,
                                       (size_t *)a4, (const void *)a5, (size_t)a6);

        case DARWIN_SYS_stat64:
        case DARWIN_SYS_lstat64:
            return mac_fs_stat((const char *)a1, (mac_stat_t *)a2);

        case DARWIN_SYS_fstat64:
            return mac_fs_fstat((int)a1, (mac_stat_t *)a2);

        case DARWIN_SYS_getxattr:
            return mac_fs_getxattr((const char *)a1, (const char *)a2, (void *)a3, (size_t)a4);

        case DARWIN_SYS_setxattr:
            return mac_fs_setxattr((const char *)a1, (const char *)a2, (const void *)a3, (size_t)a4, (int)a5);

        case DARWIN_SYS_listxattr:
            return mac_fs_listxattr((const char *)a1, (char *)a2, (size_t)a3);

        case DARWIN_SYS_removexattr:
            return mac_fs_removexattr((const char *)a1, (const char *)a2);

        case DARWIN_SYS_mmap:
            return (int64_t)native_syscall_invoke(HELIX_SYSCALL_MAP, a1, a2, a3, a4, a5, a6, 0);

        case DARWIN_SYS_munmap:
            return (int64_t)native_syscall_invoke(HELIX_SYSCALL_UNMAP, a1, a2, 0, 0, 0, 0, 0);

        case DARWIN_SYS_sched_yield:
            return (int64_t)native_syscall_invoke(HELIX_SYSCALL_THREAD_YIELD, 0, 0, 0, 0, 0, 0, 0);

        default:
            return -DARWIN_ENOSYS;
    }
}
