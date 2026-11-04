/* darwin_syscall.h — Darwin BSD and Mach syscall / trap definitions.
 *
 * Part of Helix Mac/Darwin compatibility subsystem.
 */

#ifndef MAC_DARWIN_SYSCALL_H
#define MAC_DARWIN_SYSCALL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Darwin syscall class encoding on x86_64 */
#define DARWIN_CLASS_MASK       0xFF000000
#define DARWIN_CLASS_NONE       0x00000000
#define DARWIN_CLASS_MACH       0x01000000
#define DARWIN_CLASS_BSD        0x02000000
#define DARWIN_CLASS_MDEP       0x03000000
#define DARWIN_SYSCALL_MASK     0x00FFFFFF

/* Common Darwin BSD Syscall Numbers */
#define DARWIN_SYS_exit                 1
#define DARWIN_SYS_fork                 2
#define DARWIN_SYS_read                 3
#define DARWIN_SYS_write                4
#define DARWIN_SYS_open                 5
#define DARWIN_SYS_close                6
#define DARWIN_SYS_wait4                7
#define DARWIN_SYS_link                 9
#define DARWIN_SYS_unlink               10
#define DARWIN_SYS_chdir                12
#define DARWIN_SYS_fchdir               13
#define DARWIN_SYS_mknod                14
#define DARWIN_SYS_chmod                15
#define DARWIN_SYS_chown                16
#define DARWIN_SYS_getpid               20
#define DARWIN_SYS_setuid               23
#define DARWIN_SYS_getuid               24
#define DARWIN_SYS_geteuid              25
#define DARWIN_SYS_access               33
#define DARWIN_SYS_kill                 37
#define DARWIN_SYS_getppid              39
#define DARWIN_SYS_dup                  41
#define DARWIN_SYS_pipe                 42
#define DARWIN_SYS_getegid              43
#define DARWIN_SYS_getgid               47
#define DARWIN_SYS_sigaction            48
#define DARWIN_SYS_ioctl                54
#define DARWIN_SYS_execve               59
#define DARWIN_SYS_munmap               73
#define DARWIN_SYS_mprotect             74
#define DARWIN_SYS_madvise              75
#define DARWIN_SYS_dup2                 90
#define DARWIN_SYS_fcntl                92
#define DARWIN_SYS_select               93
#define DARWIN_SYS_socket               97
#define DARWIN_SYS_connect              98
#define DARWIN_SYS_bind                 104
#define DARWIN_SYS_setsockopt           105
#define DARWIN_SYS_listen               106
#define DARWIN_SYS_gettimeofday         116
#define DARWIN_SYS_writev               121
#define DARWIN_SYS_sendto               133
#define DARWIN_SYS_mkdir                136
#define DARWIN_SYS_rmdir                137
#define DARWIN_SYS_mmap                 197
#define DARWIN_SYS_lseek                199
#define DARWIN_SYS___sysctl             202
#define DARWIN_SYS_getattrlist          220
#define DARWIN_SYS_setattrlist          221
#define DARWIN_SYS_getxattr             234
#define DARWIN_SYS_fgetxattr            235
#define DARWIN_SYS_setxattr             236
#define DARWIN_SYS_fsetxattr            237
#define DARWIN_SYS_removexattr          238
#define DARWIN_SYS_fremovexattr         239
#define DARWIN_SYS_listxattr            240
#define DARWIN_SYS_flistxattr           241
#define DARWIN_SYS_sched_yield          327
#define DARWIN_SYS_stat64               338
#define DARWIN_SYS_fstat64              339
#define DARWIN_SYS_lstat64              340
#define DARWIN_SYS_getdirentries64      344
#define DARWIN_SYS_bsdthread_create     360
#define DARWIN_SYS_bsdthread_terminate  361
#define DARWIN_SYS_proc_info            397

/* Mach Trap Numbers */
#define MACH_TRAP_vm_allocate           10
#define MACH_TRAP_vm_deallocate         12
#define MACH_TRAP_vm_protect            14
#define MACH_TRAP_vm_map                15
#define MACH_TRAP_port_allocate         18
#define MACH_TRAP_port_destroy          19
#define MACH_TRAP_port_deallocate       20
#define MACH_TRAP_port_insert_right     24
#define MACH_TRAP_reply_port            26
#define MACH_TRAP_thread_self           27
#define MACH_TRAP_task_self             28
#define MACH_TRAP_host_self             29
#define MACH_TRAP_msg                   31
#define MACH_TRAP_msg_overwrite         32
#define MACH_TRAP_semaphore_signal      36
#define MACH_TRAP_semaphore_wait        37
#define MACH_TRAP_thread_switch         61
#define MACH_TRAP_timebase_info         89

/* Darwin Error Codes */
#define DARWIN_EPERM                    1
#define DARWIN_ENOENT                   2
#define DARWIN_ESRCH                    3
#define DARWIN_EINTR                    4
#define DARWIN_EIO                      5
#define DARWIN_EBADF                    9
#define DARWIN_ENOMEM                   12
#define DARWIN_EACCES                   13
#define DARWIN_EFAULT                   14
#define DARWIN_EEXIST                   17
#define DARWIN_ENODEV                   19
#define DARWIN_ENOTDIR                  20
#define DARWIN_EISDIR                   21
#define DARWIN_EINVAL                   22
#define DARWIN_ENFILE                   23
#define DARWIN_EMFILE                   24
#define DARWIN_ENOTTY                   25
#define DARWIN_EFBIG                    27
#define DARWIN_ENOSPC                   28
#define DARWIN_ESPIPE                   29
#define DARWIN_EROFS                    30
#define DARWIN_EPIPE                    32
#define DARWIN_EDOM                     33
#define DARWIN_ERANGE                   34
#define DARWIN_EAGAIN                   35
#define DARWIN_ENOSYS                   78

/* Sysctl Top-Level Names */
#define CTL_SYSCTL                      0
#define CTL_KERN                        1
#define CTL_VM                          2
#define CTL_VFS                         3
#define CTL_NET                         4
#define CTL_DEBUG                       5
#define CTL_HW                          6
#define CTL_MACHDEP                     7

/* Sysctl KERN Names */
#define KERN_OSTYPE                     1
#define KERN_OSRELEASE                  2
#define KERN_OSREV                      3
#define KERN_VERSION                    4
#define KERN_MAXVNODES                  5
#define KERN_MAXPROC                    6
#define KERN_HOSTNAME                   10
#define KERN_HOSTID                     11
#define KERN_BOOTTIME                   21
#define KERN_PROC                       14
#define KERN_USRSTACK64                 35

/* Sysctl HW Names */
#define HW_MACHINE                      1
#define HW_MODEL                        2
#define HW_NCPU                         3
#define HW_BYTEORDER                    4
#define HW_PHYSMEM                      5
#define HW_USERMEM                      6
#define HW_PAGESIZE                     7
#define HW_MEMSIZE                      24
#define HW_CPU_FAMILY                   25

/* Darwin Syscall Translation API */
int64_t darwin_translate_number(uint64_t raw_num, bool *out_is_mach);
int64_t darwin_syscall_dispatch(uint64_t num, uint64_t a1, uint64_t a2,
                                uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6);
int darwin_sysctl_query(const int *name, uint32_t namelen, void *oldp,
                        size_t *oldlenp, const void *newp, size_t newlen);
int darwin_sysctlbyname(const char *name, void *oldp, size_t *oldlenp,
                        const void *newp, size_t newlen);

#ifdef __cplusplus
}
#endif

#endif /* MAC_DARWIN_SYSCALL_H */
