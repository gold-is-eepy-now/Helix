/* fat32.h — read-only FAT32 driver for Helix OS.
 *
 * Written from Microsoft's "FAT: General Overview of On-Disk Format" (the boot
 * sector fields, the cluster arithmetic, the directory entry and long-name
 * layouts). No other kernel's source was used.
 *
 * Stage 1 scope:
 *   - read only; mounts a FAT32 volume that starts at a given LBA on a block device
 *     (use 0 for a whole-disk image; partition tables are not parsed yet)
 *   - long file names (VFAT) are read and converted to UTF-8; names are matched
 *     case-insensitively for ASCII letters only
 *   - FAT12 and FAT16 are refused, as is anything the spec says is not FAT32
 *
 * Callbacks passed to fat32_list must not call back into the same filesystem: the
 * driver reuses one cluster buffer for directory scans.
 */
#ifndef HELIX_FAT32_H
#define HELIX_FAT32_H

#include <stdbool.h>
#include <stdint.h>

typedef struct block_device block_device_t;
typedef struct fat32_fs fat32_fs_t;

/* Longest name returned: 255 UTF-16 units can need up to 3 bytes each in UTF-8,
 * and a name that does not fit is cut at this length. */
#define FAT32_NAME_BYTES 256

/* Directory entry attribute bits. */
#define FAT32_ATTR_READ_ONLY 0x01
#define FAT32_ATTR_HIDDEN    0x02
#define FAT32_ATTR_SYSTEM    0x04
#define FAT32_ATTR_VOLUME_ID 0x08
#define FAT32_ATTR_DIRECTORY 0x10
#define FAT32_ATTR_ARCHIVE   0x20

typedef struct {
    char name[FAT32_NAME_BYTES];   /* long name if present, otherwise the 8.3 name */
    bool is_dir;
    uint8_t attr;
    uint32_t size;                 /* bytes; 0 for directories */
    uint32_t first_cluster;
} fat32_entry_t;

/* Called once per directory entry. Return true to stop the scan early. */
typedef bool (*fat32_dirent_cb)(void *ctx, const fat32_entry_t *entry);

/* Mount the FAT32 volume whose boot sector is at first_lba. Returns NULL if the device
 * does not hold a usable FAT32 volume; fat32_mount_error() then says why. */
fat32_fs_t *fat32_mount(block_device_t *dev, uint64_t first_lba);
void fat32_unmount(fat32_fs_t *fs);
const char *fat32_mount_error(void);

/* Volume label (from the root directory, or the boot sector if there is none). */
const char *fat32_label(fat32_fs_t *fs);

/* Geometry and usage. free_clusters is counted by scanning the FAT, so it is not free. */
void fat32_info(fat32_fs_t *fs, uint32_t *bytes_per_sector, uint32_t *sectors_per_cluster,
                uint32_t *clusters, uint32_t *free_clusters);

/* Look up a path such as "/DOCS/NESTED.TXT". "/" is the root directory. */
bool fat32_stat(fat32_fs_t *fs, const char *path, fat32_entry_t *out);

/* List a directory. Returns false if the path does not exist or is not a directory. */
bool fat32_list(fat32_fs_t *fs, const char *path, fat32_dirent_cb cb, void *ctx);

/* Read up to len bytes of a file starting at offset. Returns the byte count (0 at or past
 * the end of the file), or -1 if the path is missing, is a directory, or a read fails. */
int64_t fat32_read(fat32_fs_t *fs, const char *path, uint64_t offset, void *buf, uint64_t len);

#endif
