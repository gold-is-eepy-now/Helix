/* mac_fs.h — Mac-like Filesystem (HFS+/APFS emulation).
 *
 * Case-insensitive, case-preserving path lookup, macOS directory hierarchy,
 * extended attributes (xattrs), and resource fork / bundle support.
 *
 * Part of Helix Mac/Darwin compatibility subsystem.
 */

#ifndef MAC_DARWIN_FS_H
#define MAC_DARWIN_FS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAC_MAX_PATH           1024
#define MAC_MAX_FILENAME       255
#define MAC_MAX_NODES          256
#define MAC_MAX_XATTRS        8
#define MAC_MAX_XATTR_KEY      64
#define MAC_MAX_XATTR_VAL      256
#define MAC_MAX_OPEN_FILES     64

/* Node types */
typedef enum {
    MAC_NODE_FILE = 1,
    MAC_NODE_DIR,
    MAC_NODE_SYMLINK,
} mac_node_type_t;

/* Extended Attribute */
typedef struct {
    char key[MAC_MAX_XATTR_KEY];
    uint8_t value[MAC_MAX_XATTR_VAL];
    size_t size;
    bool in_use;
} mac_xattr_t;

/* File Stat */
typedef struct {
    uint64_t st_dev;
    uint64_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    uint64_t st_size;
    uint64_t st_atime;
    uint64_t st_mtime;
    uint64_t st_ctime;
    uint64_t st_birthtime;
    uint32_t st_blksize;
    uint32_t st_flags;
} mac_stat_t;

/* Directory Entry for readdir */
typedef struct {
    uint64_t d_ino;
    uint16_t d_reclen;
    uint8_t  d_type;
    uint8_t  d_namlen;
    char     d_name[MAC_MAX_FILENAME + 1];
} mac_dirent_t;

/* VFS Node */
typedef struct mac_vnode {
    uint64_t        ino;
    char            name[MAC_MAX_FILENAME + 1];
    mac_node_type_t type;
    uint32_t        mode;
    uint32_t        uid;
    uint32_t        gid;
    uint8_t        *content;
    size_t          size;
    size_t          capacity;
    char            target[MAC_MAX_PATH]; /* For symlink */
    struct mac_vnode *parent;
    struct mac_vnode *children;
    struct mac_vnode *next_sibling;
    mac_xattr_t     xattrs[MAC_MAX_XATTRS];
    uint32_t        xattr_count;
    uint64_t        mtime;
} mac_vnode_t;

/* File Descriptor */
typedef struct {
    mac_vnode_t *node;
    uint64_t     offset;
    int          flags;
    bool         in_use;
} mac_file_desc_t;

/* Public Mac Filesystem API */
void         mac_fs_init(void);
void         mac_fs_teardown(void);
mac_vnode_t *mac_fs_resolve_path(const char *path, bool follow_symlinks);
mac_vnode_t *mac_fs_mkdir(const char *path, uint32_t mode);
mac_vnode_t *mac_fs_create_file(const char *path, uint32_t mode);
mac_vnode_t *mac_fs_create_symlink(const char *path, const char *target);
int          mac_fs_write_file_data(const char *path, const void *data, size_t len);
int          mac_fs_read_file_data(const char *path, void *buf, size_t maxlen, size_t *out_len);
int          mac_fs_stat(const char *path, mac_stat_t *st);

/* Open File Descriptor API */
int          mac_fs_open(const char *path, int flags, uint32_t mode);
int64_t      mac_fs_read(int fd, void *buf, size_t count);
int64_t      mac_fs_write(int fd, const void *buf, size_t count);
int64_t      mac_fs_lseek(int fd, int64_t offset, int whence);
int          mac_fs_close(int fd);
int          mac_fs_fstat(int fd, mac_stat_t *st);
int          mac_fs_readdir(const char *path, mac_dirent_t *entries, size_t max_entries);

/* Extended Attribute (xattr) API */
int          mac_fs_setxattr(const char *path, const char *name, const void *value, size_t size, int flags);
int          mac_fs_getxattr(const char *path, const char *name, void *value, size_t size);
int          mac_fs_listxattr(const char *path, char *namebuf, size_t size);
int          mac_fs_removexattr(const char *path, const char *name);

#ifdef __cplusplus
}
#endif

#endif /* MAC_DARWIN_FS_H */
