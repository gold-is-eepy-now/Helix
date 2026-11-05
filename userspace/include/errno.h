/* errno.h — error numbers for the Helix C runtime (Linux values). */
#ifndef HELIX_LIBC_ERRNO_H
#define HELIX_LIBC_ERRNO_H

extern int errno;

#define EPERM   1
#define ENOENT  2
#define EIO     5
#define EBADF   9
#define ENOMEM  12
#define EEXIST  17
#define EINVAL  22
#define ENOSPC  28

#endif
