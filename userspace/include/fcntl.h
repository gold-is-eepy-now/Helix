/* fcntl.h — open() flags (same values as the kernel's UFILE_O_*). */
#ifndef HELIX_LIBC_FCNTL_H
#define HELIX_LIBC_FCNTL_H

#include <unistd.h>

#define O_RDONLY 0x0
#define O_WRONLY 0x1
#define O_RDWR   0x2
#define O_CREAT  0x40
#define O_TRUNC  0x200

int open(const char *path, int flags);

#endif
