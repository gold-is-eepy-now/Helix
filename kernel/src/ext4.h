/* ext4.h — read/write ext4 driver for Helix OS.
 *
 * Mounted on any block_device_t. Supports the ext4 features mke2fs turns on
 * by default: extents, 64-bit group descriptors, filetype directory entries,
 * flex_bg, extra_isize. Journal replay, bigalloc, inline data and encryption
 * are refused at mount.
 *
 * Inode numbers are 64-bit (ext4's 32-bit inode numbers widened). All offsets
 * and sizes are bytes unless a name says otherwise.
 */
#ifndef HELIX_EXT4_H
#define HELIX_EXT4_H

#include <stdbool.h>
#include <stdint.h>

typedef struct block_device block_device_t;
typedef struct ext4_fs ext4_fs_t;

#define EXT4_NAME_MAX 255

/* Root inode number for ext4. */
#define EXT4_ROOT_INO 2u

/* Mode bits, shaped like POSIX so VFS code reads naturally. */
#define EXT4_S_IFMT  0xF000
#define EXT4_S_IFREG 0x8000
#define EXT4_S_IFDIR 0x4000
#define EXT4_S_IFLNK 0xA000
#define EXT4_S_ISREG(m) (((m) & EXT4_S_IFMT) == EXT4_S_IFREG)
#define EXT4_S_ISDIR(m) (((m) & EXT4_S_IFMT) == EXT4_S_IFDIR)
#define EXT4_S_ISLNK(m) (((m) & EXT4_S_IFMT) == EXT4_S_IFLNK)

/* File type byte stored in directory entries when INCOMPAT_FILETYPE is set. */
#define EXT4_FT_UNKNOWN  0
#define EXT4_FT_REG_FILE 1
#define EXT4_FT_DIR      2
#define EXT4_FT_SYMLINK  7

typedef struct {
    uint64_t ino;
    uint16_t mode;
    uint64_t size;
    uint16_t links;
    uint32_t mtime;
} ext4_stat_t;

/* Called once per directory entry. Return true to stop the scan early. */
typedef bool (*ext4_dirent_cb)(void *ctx, const ext4_stat_t *st,
                               const char *name, uint32_t name_len);

/* Mount/unmount. Returns NULL if the device is not a usable ext4 volume. */
ext4_fs_t *ext4_mount(block_device_t *dev, bool read_only);
void ext4_unmount(ext4_fs_t *fs);

uint64_t ext4_root(ext4_fs_t *fs);
const char *ext4_label(ext4_fs_t *fs);
block_device_t *ext4_device(ext4_fs_t *fs);
void ext4_space(ext4_fs_t *fs, uint64_t *total_bytes, uint64_t *free_bytes,
                uint64_t *total_inodes, uint64_t *free_inodes);

/* Metadata. */
bool ext4_stat(ext4_fs_t *fs, uint64_t ino, ext4_stat_t *out);
bool ext4_lookup(ext4_fs_t *fs, uint64_t dir_ino, const char *name,
                 ext4_stat_t *out);
uint64_t ext4_readdir(ext4_fs_t *fs, uint64_t dir_ino, ext4_dirent_cb cb,
                      void *ctx);

/* File data. Returns bytes transferred; reads of holes return zeros. */
uint64_t ext4_read(ext4_fs_t *fs, uint64_t ino, uint64_t offset, void *buf,
                   uint64_t len);
uint64_t ext4_write(ext4_fs_t *fs, uint64_t ino, uint64_t offset,
                    const void *buf, uint64_t len);

/* Namespace changes. */
bool ext4_create(ext4_fs_t *fs, uint64_t dir_ino, const char *name,
                 uint16_t mode, uint64_t *out_ino);
bool ext4_mkdir(ext4_fs_t *fs, uint64_t dir_ino, const char *name,
                uint16_t mode, uint64_t *out_ino);
bool ext4_unlink(ext4_fs_t *fs, uint64_t dir_ino, const char *name,
                 bool as_dir);
bool ext4_truncate(ext4_fs_t *fs, uint64_t ino, uint64_t size);

/* Seconds used for inode timestamps: seconds since boot.
 * ponytail: no wall clock yet; swap for a real epoch when RTC/NTP lands. */
void ext4_set_time_source(uint32_t (*seconds)(void));

/* Return the currently mounted ext4 filesystem, if any. */
ext4_fs_t *ext4_get_current(void);

#endif /* HELIX_EXT4_H */
