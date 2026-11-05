/* ufile.h — file descriptors for the running user process, backed by ext4.
 *
 * A user program names files by absolute path ("/bin/sh") and reads or writes
 * them through small integer descriptors, as on Unix. The table lives in the
 * kernel for the running process: the kernel keeps the inode and the read/write
 * position, so a process cannot forge an inode number.
 *
 * Scope (M9): absolute paths only, no directories opened as files, no
 * permissions, no dup/pipes. Descriptors 0-2 are the console and are not in
 * this table. The table is reset when a process starts and when it exits. */
#ifndef HELIX_UFILE_H
#define HELIX_UFILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* open(2) flag values; these match Linux so they read the same in libhelix. */
#define UFILE_O_RDONLY 0x0
#define UFILE_O_WRONLY 0x1
#define UFILE_O_RDWR   0x2
#define UFILE_O_ACCMODE 0x3
#define UFILE_O_CREAT  0x40
#define UFILE_O_TRUNC  0x200

#define UFILE_SEEK_SET 0
#define UFILE_SEEK_CUR 1
#define UFILE_SEEK_END 2

#define UFILE_MAX_FDS 16

/* One open file. Used is false for a free slot. */
typedef struct {
    bool used;
    uint64_t ino;
    uint64_t pos;
    bool readable;
    bool writable;
} ufd_t;

/* A copy of the descriptor table, so a nested launch can restore the caller's. */
typedef struct {
    ufd_t fds[UFILE_MAX_FDS];
} ufile_state_t;

/* Copy the table out, or back in. */
void ufile_save(ufile_state_t *st);
void ufile_restore(const ufile_state_t *st);

/* Read a whole regular file at an absolute path into kernel memory (kmalloc, so
 * the caller frees it). Files above 1 MiB are refused. */
bool ufile_load_file(const char *path, uint8_t **out, uint64_t *out_len);

/* Forget every open descriptor. */
void ufile_reset(void);

/* Open an absolute path that is already in kernel memory. Returns the new
 * descriptor (3 or more), or -1. */
int64_t ufile_open(const char *path, uint64_t flags);

/* Read or write up to `len` bytes at the descriptor's position. Return the byte
 * count (0 at end of file), or -1 for a bad descriptor or wrong access mode. */
int64_t ufile_read(int64_t fd, void *dst, uint64_t len);
int64_t ufile_write(int64_t fd, const void *src, uint64_t len);

/* Move the position. Returns the new offset, or -1. */
int64_t ufile_seek(int64_t fd, int64_t offset, int whence);

/* Release a descriptor. Returns 0, or -1 if it was not open. */
int64_t ufile_close(int64_t fd);

#endif /* HELIX_UFILE_H */
