/* filesystem.h — native Helix filesystem interface.
 *
 * M11 milestone: storage and filesystems.
 *
 * The Helix filesystem is designed around objects, not inodes-as-fds.
 * A filesystem instance is an object that provides file lookup, read, write,
 * and directory listing operations through a uniform interface.
 *
 * Filesystems are mounted at mount points and accessed by path. The boot
 * filesystem is a read-only filesystem that loads from a Limine module
 * or in-memory image.
 */

#ifndef HELIX_FILESYSTEM_H
#define HELIX_FILESYSTEM_H

#include "object.h"

#define FS_MAX_PATH 256
#define FS_MAX_NAME 64
#define FS_MAX_FILES 64
#define FS_BLOCK_SIZE 4096

typedef struct file_entry file_entry_t;
typedef struct filesystem filesystem_t;

/* File type flags */
#define FS_FILE  0x01
#define FS_DIR   0x02
#define FS_READ  0x04
#define FS_WRITE 0x08

struct file_entry {
    char name[FS_MAX_NAME];
    uint32_t type;
    uint64_t size;
    uint64_t pos;
    void *data;          /* file data (for in-memory files) */
    filesystem_t *fs;    /* back-pointer to filesystem */
    file_entry_t *parent;
    file_entry_t *sibling;
    file_entry_t *children;
    object_t *object;
};

struct filesystem {
    const char *name;
    const char *mount_point;
    file_entry_t *root;
    object_t *fs_object;
    bool read_only;
    filesystem_t *next;
};

/* Core filesystem operations */
filesystem_t *fs_mount(const char *device_name, const char *mount_point,
                       const char *fs_name, bool read_only);
void fs_unmount(const char *mount_point);
filesystem_t *fs_find_mount(const char *path);
file_entry_t *fs_open(const char *path);
void fs_close(file_entry_t *file);
uint64_t fs_read(file_entry_t *file, void *buffer, uint64_t count);
uint64_t fs_write(file_entry_t *file, const void *buffer, uint64_t count);

/* Filesystem registration */
typedef void (*fs_mount_fn)(filesystem_t *fs);
typedef file_entry_t *(*fs_open_fn)(filesystem_t *fs, const char *path);
typedef uint64_t (*fs_read_fn)(file_entry_t *file, void *buffer, uint64_t count);

void fs_register_mount_handler(const char *fs_name, fs_mount_fn mount);
void fs_register_open_handler(const char *fs_name, fs_open_fn open);
void fs_register_read_handler(const char *fs_name, fs_read_fn read);

/* Boot filesystem operations */
void fs_boot_init(void);
void fs_boot_add_file(const char *name, const void *data, uint64_t size);
const char *fs_boot_root(void);

/* List files in a directory */
uint64_t fs_list_dir(file_entry_t *dir, file_entry_t **out_entries,
                     uint64_t max_entries);

/* Get file info */
uint64_t fs_file_size(file_entry_t *file);
bool fs_file_is_dir(file_entry_t *file);

#endif /* HELIX_FILESYSTEM_H */