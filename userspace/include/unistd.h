/* unistd.h — file descriptors and process exit for the Helix C runtime. */
#ifndef HELIX_LIBC_UNISTD_H
#define HELIX_LIBC_UNISTD_H

#include <stddef.h>

typedef long ssize_t;
typedef long off_t;

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
int close(int fd);
off_t lseek(int fd, off_t offset, int whence);
__attribute__((noreturn)) void _exit(int status);

/* The environment: NULL-terminated "NAME=value" strings. The C runtime points it at
 * the block the kernel built on the stack; setenv/unsetenv move it to the heap. */
extern char **environ;

#endif
