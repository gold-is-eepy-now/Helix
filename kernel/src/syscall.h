/* syscall.h — the first small native Helix system-call ABI. */
#ifndef HELIX_SYSCALL_H
#define HELIX_SYSCALL_H

#define HELIX_SYS_EXIT  0u
#define HELIX_SYS_WRITE 1u
#define HELIX_SYS_READ  2u
#define HELIX_SYS_CLEAR 3u
#define HELIX_SYS_MAX_WRITE 256u
#define HELIX_SYS_MAX_READ  128u

#endif /* HELIX_SYSCALL_H */
