/* unistd.c — file descriptors over the kernel's system calls (see unistd.h).
 *
 * Descriptor 0 is the console, read with SYS_READ_INPUT: read() on it waits for
 * a key and returns what is typed up to and including a newline. Descriptors
 * 1 and 2 go to the console for writes. Other descriptors are files. */
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "libhelix.h"

ssize_t read(int fd, void *buf, size_t count) {
    if (fd == 0) {
        if (count == 0) return 0;
        char *p = buf;
        size_t n = 0;
        for (;;) {
            int c = helix_console_getchar();
            p[n++] = (char)c;
            if (c == '\n' || n == count) return (ssize_t)n;
        }
    }
    int64_t r = helix_read(fd, buf, count);
    if (r < 0) {
        errno = EBADF;
        return -1;
    }
    return (ssize_t)r;
}

ssize_t write(int fd, const void *buf, size_t count) {
    int64_t r = helix_write(fd, buf, count);
    if (r < 0) {
        errno = EBADF;
        return -1;
    }
    return (ssize_t)r;
}

int close(int fd) {
    if (helix_close(fd) < 0) {
        errno = EBADF;
        return -1;
    }
    return 0;
}

off_t lseek(int fd, off_t offset, int whence) {
    int64_t r = helix_lseek(fd, (int64_t)offset, whence);
    if (r < 0) {
        errno = EINVAL;
        return -1;
    }
    return (off_t)r;
}

int open(const char *path, int flags) {
    int fd = helix_open(path, flags);
    if (fd < 0) {
        errno = ENOENT;
        return -1;
    }
    return fd;
}

void _exit(int status) {
    helix_exit(status);
}
