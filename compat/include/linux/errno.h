/* linux/errno.h — Linux error numbers. Negative values returned from init and
 * driver calls mean "failed with this error", as in Linux. */
#ifndef HELIX_COMPAT_LINUX_ERRNO_H
#define HELIX_COMPAT_LINUX_ERRNO_H

#define EPERM    1
#define ENOENT   2
#define EIO      5
#define ENXIO    6
#define ENOMEM  12
#define EBUSY   16
#define ENODEV  19
#define EINVAL  22
#define ENOSPC  28
#define ENOSYS  38
#define ETIMEDOUT 110

#endif /* HELIX_COMPAT_LINUX_ERRNO_H */
