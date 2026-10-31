/* ext4.c — read/write ext4 driver for Helix OS.
 *
 * Scope: superblock, 32/64-bit group descriptors, inodes, extent trees,
 * block-mapped (ext2-style) read, filetype directory entries, plus create /
 * write / truncate / unlink so the installer can populate a volume.
 *
 * Not supported (refused at mount): journal replay, bigalloc, inline data,
 * encryption, casefold, meta_bg. Checksums are not recomputed on write, so a
 * volume mounted read-write is not e2fsck-clean after our writes
 * (ponytail: add metadata_csum updates when a writable volume must pass fsck).
 */
#include "ext4.h"

#include <stddef.h>

#include "block_device.h"
#include "console.h"
#include "heap.h"
#include "kstring.h"

#define EXT4_SUPER_OFF 1024u
#define EXT4_SUPER_LEN 1024u
#define EXT4_MAGIC 0xEF53u
#define EXT4_ROOT_INO 2u
#define EXT4_INODE_MIN 128u
#define EXT4_EXT_MAGIC 0xF30Au

#define EXT4_EXTENTS_FL 0x00080000u
#define EXT4_HUGE_FILE_FL 0x00040000u

#define INCOMPAT_COMPRESSION 0x0001u
#define INCOMPAT_FILETYPE 0x0002u
#define INCOMPAT_RECOVER 0x0004u
#define INCOMPAT_JOURNAL_DEV 0x0008u
#define INCOMPAT_META_BG 0x0010u
#define INCOMPAT_EXTENTS 0x0040u
#define INCOMPAT_64BIT 0x0080u
#define INCOMPAT_MMP 0x0100u
#define INCOMPAT_FLEX_BG 0x0200u
#define INCOMPAT_DIRDATA 0x1000u
#define INCOMPAT_INLINE_DATA 0x8000u
#define INCOMPAT_ENCRYPT 0x10000u
#define INCOMPAT_CASEFOLD 0x20000u

#define RO_BIGALLOC 0x0200u
#define RO_METADATA_CSUM 0x0400u

#define EXT4_BG_INODE_UNINIT 0x0001u
#define EXT4_BG_BLOCK_UNINIT 0x0002u

typedef struct __attribute__((packed)) {
    uint32_t inodes_count;
    uint32_t blocks_count_lo;
    uint32_t r_blocks_count_lo;
    uint32_t free_blocks_count_lo;
    uint32_t free_inodes_count;
    uint32_t first_data_block;
    uint32_t log_block_size;
    uint32_t log_cluster_size;
    uint32_t blocks_per_group;
    uint32_t clusters_per_group;
    uint32_t inodes_per_group;
    uint32_t mtime;
    uint32_t wtime;
    uint16_t mnt_count;
    uint16_t max_mnt_count;
    uint16_t magic;
    uint16_t state;
    uint16_t errors;
    uint16_t minor_rev_level;
    uint32_t lastcheck;
    uint32_t checkinterval;
    uint32_t creator_os;
    uint32_t rev_level;
    uint16_t def_resuid;
    uint16_t def_resgid;
    uint32_t first_ino;
    uint16_t inode_size;
    uint16_t block_group_nr;
    uint32_t feature_compat;
    uint32_t feature_incompat;
    uint32_t feature_ro_compat;
    uint8_t uuid[16];
    char volume_name[16];
    char last_mounted[64];
    uint32_t algorithm_usage_bitmap;
    uint8_t prealloc_blocks;
    uint8_t prealloc_dir_blocks;
    uint16_t reserved_gdt_blocks;
    uint8_t journal_uuid[16];
    uint32_t journal_inum;
    uint32_t journal_dev;
    uint32_t last_orphan;
    uint32_t hash_seed[4];
    uint8_t def_hash_version;
    uint8_t jnl_backup_type;
    uint16_t desc_size;
    uint32_t default_mount_opts;
    uint32_t first_meta_bg;
    uint32_t mkfs_time;
    uint32_t jnl_blocks[17];
    uint32_t blocks_count_hi;
    uint32_t r_blocks_count_hi;
    uint32_t free_blocks_count_hi;
    uint16_t min_extra_isize;
    uint16_t want_extra_isize;
    uint32_t flags;
} ext4_sb_t;

typedef struct __attribute__((packed)) {
    uint32_t block_bitmap_lo;
    uint32_t inode_bitmap_lo;
    uint32_t inode_table_lo;
    uint16_t free_blocks_count_lo;
    uint16_t free_inodes_count_lo;
    uint16_t used_dirs_count_lo;
    uint16_t flags;
    uint32_t exclude_bitmap_lo;
    uint16_t block_bitmap_csum_lo;
    uint16_t inode_bitmap_csum_lo;
    uint16_t itable_unused_lo;
    uint16_t checksum;
    uint32_t block_bitmap_hi;
    uint32_t inode_bitmap_hi;
    uint32_t inode_table_hi;
    uint16_t free_blocks_count_hi;
    uint16_t free_inodes_count_hi;
    uint16_t used_dirs_count_hi;
    uint16_t itable_unused_hi;
    uint32_t exclude_bitmap_hi;
    uint16_t block_bitmap_csum_hi;
    uint16_t inode_bitmap_csum_hi;
} ext4_gd_t; /* 60 bytes; desc_size is 32 (old) or 64 (64bit) */

typedef struct __attribute__((packed)) {
    uint16_t mode;
    uint16_t uid;
    uint32_t size_lo;
    uint32_t atime;
    uint32_t ctime;
    uint32_t mtime;
    uint32_t dtime;
    uint16_t gid;
    uint16_t links;
    uint32_t blocks_lo;
    uint32_t flags;
    uint32_t osd1;
    uint8_t i_block[60];
    uint32_t generation;
    uint32_t file_acl_lo;
    uint32_t size_high;
    uint32_t obso_faddr;
    uint16_t blocks_high;
    uint16_t file_acl_high;
    uint16_t uid_high;
    uint16_t gid_high;
    uint16_t checksum_lo;
    uint16_t reserved;
} ext4_inode_t; /* exactly 128 bytes */

typedef struct __attribute__((packed)) {
    uint16_t eh_magic;
    uint16_t eh_entries;
    uint16_t eh_max;
    uint16_t eh_depth;
    uint32_t eh_generation;
} ext4_eh_t;

typedef struct __attribute__((packed)) {
    uint32_t ee_block;
    uint16_t ee_len;
    uint16_t ee_start_hi;
    uint32_t ee_start_lo;
} ext4_eent_t;

typedef struct __attribute__((packed)) {
    uint32_t ei_block;
    uint32_t ei_leaf_lo;
    uint16_t ei_leaf_hi;
    uint16_t ei_unused;
} ext4_eidx_t;

typedef struct {
    uint32_t logical;  /* logical block in the directory */
    uint32_t offset;   /* byte offset of this entry in that block */
    uint32_t rec_len;
    uint32_t inode;
    uint8_t name_len;
    uint8_t ftype;
    char name[EXT4_NAME_MAX + 1];
} ext4_dent_t;

struct ext4_fs {
    block_device_t *dev;
    uint32_t block_size;
    uint32_t blocks_per_group;
    uint32_t inodes_per_group;
    uint32_t inode_size;
    uint32_t desc_size;
    uint32_t groups;
    uint32_t first_data_block;
    uint32_t first_ino;
    uint64_t blocks_count;
    uint64_t inodes_count;
    uint32_t incompat;
    uint32_t ro_compat;
    bool read_only;
    bool has_extents;
    char label[17];
    ext4_sb_t *sb;
    uint8_t *gds;
    void *b_data;    /* the directory/file block being processed */
    void *b_leaf;    /* an external extent/indirect block */
    void *b_bitmap;  /* a block or inode bitmap */
    void *b_inode;   /* one full inode image (inode_size bytes) */
};

static uint32_t (*g_time_source)(void);
static ext4_fs_t *g_ext4_current = NULL;

void ext4_set_time_source(uint32_t (*seconds)(void)) {
    g_time_source = seconds;
}

static uint32_t now_secs(void) {
    return g_time_source ? g_time_source() : 0;
}

ext4_fs_t *ext4_get_current(void) {
    return g_ext4_current;
}

/* ------------------------------------------------------------------ */
/* Byte-level device IO                                                */
/* ------------------------------------------------------------------ */

static bool dev_io(ext4_fs_t *fs, uint64_t off, void *buf, uint64_t len,
                   bool write) {
    uint64_t bs = fs->dev->block_size;
    uint8_t *p = buf;
    uint64_t lba = off / bs;
    uint64_t skip = off % bs;
    static uint8_t sector[4096];

    if (bs == 0 || bs > sizeof(sector)) return false;

    while (len > 0) {
        uint64_t chunk = bs - skip;
        if (chunk > len) chunk = len;
        if (skip == 0 && chunk == bs) {
            uint64_t got = write
                ? block_device_write(fs->dev, lba, p, 1)
                : block_device_read(fs->dev, lba, p, 1);
            if (got != 1) return false;
        } else {
            if (write) {
                if (block_device_read(fs->dev, lba, sector, 1) != 1) return false;
                memcpy(sector + skip, p, chunk);
                if (block_device_write(fs->dev, lba, sector, 1) != 1) return false;
            } else {
                if (block_device_read(fs->dev, lba, sector, 1) != 1) return false;
                memcpy(p, sector + skip, chunk);
            }
        }
        p += chunk;
        len -= chunk;
        lba++;
        skip = 0;
    }
    return true;
}

static bool read_bytes(ext4_fs_t *fs, uint64_t off, void *buf, uint64_t len) {
    return dev_io(fs, off, buf, len, false);
}

static bool write_bytes(ext4_fs_t *fs, uint64_t off, const void *buf,
                        uint64_t len) {
    return dev_io(fs, off, (void *)buf, len, true);
}

static bool read_block(ext4_fs_t *fs, uint32_t blk, void *buf) {
    return read_bytes(fs, (uint64_t)blk * fs->block_size, buf, fs->block_size);
}

static bool write_block(ext4_fs_t *fs, uint32_t blk, const void *buf) {
    if (fs->read_only) return false;
    return write_bytes(fs, (uint64_t)blk * fs->block_size, buf,
                       fs->block_size);
}

/* ------------------------------------------------------------------ */
/* Superblock and group descriptors                                    */
/* ------------------------------------------------------------------ */

static ext4_sb_t *SB(ext4_fs_t *fs) { return fs->sb; }

static ext4_gd_t *GD(ext4_fs_t *fs, uint32_t g) {
    return (ext4_gd_t *)(fs->gds + (size_t)g * fs->desc_size);
}

static uint64_t sb_blocks(ext4_fs_t *fs) {
    ext4_sb_t *sb = SB(fs);
    if (fs->incompat & INCOMPAT_64BIT) {
        return (uint64_t)sb->blocks_count_lo | ((uint64_t)sb->blocks_count_hi << 32);
    }
    return sb->blocks_count_lo;
}

static uint64_t sb_free_blocks(ext4_fs_t *fs) {
    ext4_sb_t *sb = SB(fs);
    if (fs->incompat & INCOMPAT_64BIT) {
        return (uint64_t)sb->free_blocks_count_lo |
               ((uint64_t)sb->free_blocks_count_hi << 32);
    }
    return sb->free_blocks_count_lo;
}

static void sb_set_free_blocks(ext4_fs_t *fs, uint64_t v) {
    ext4_sb_t *sb = SB(fs);
    sb->free_blocks_count_lo = (uint32_t)v;
    if (fs->incompat & INCOMPAT_64BIT) {
        sb->free_blocks_count_hi = (uint32_t)(v >> 32);
    }
}

static bool sb_flush(ext4_fs_t *fs) {
    if (fs->read_only) return false;
    return write_bytes(fs, EXT4_SUPER_OFF, fs->sb, EXT4_SUPER_LEN);
}

static bool gd_flush(ext4_fs_t *fs, uint32_t g) {
    if (fs->read_only) return false;
    uint64_t gd_off = (uint64_t)(fs->first_data_block + 1) * fs->block_size;
    return write_bytes(fs, gd_off + (uint64_t)g * fs->desc_size,
                       fs->gds + (size_t)g * fs->desc_size, fs->desc_size);
}

/* 64-bit group descriptor fields are only present when the descriptor is
 * 64 bytes long; reading them from a 32-byte descriptor would run into the
 * next one (or past the array), so the guard lives in each accessor. */
static bool gd_has_hi(ext4_fs_t *fs) {
    return (fs->incompat & INCOMPAT_64BIT) != 0 && fs->desc_size >= 64;
}

static uint64_t gd_block_bitmap(ext4_fs_t *fs, ext4_gd_t *gd) {
    uint64_t v = gd->block_bitmap_lo;
    if (gd_has_hi(fs)) v |= (uint64_t)gd->block_bitmap_hi << 32;
    return v;
}
static uint64_t gd_inode_bitmap(ext4_fs_t *fs, ext4_gd_t *gd) {
    uint64_t v = gd->inode_bitmap_lo;
    if (gd_has_hi(fs)) v |= (uint64_t)gd->inode_bitmap_hi << 32;
    return v;
}
static uint64_t gd_inode_table(ext4_fs_t *fs, ext4_gd_t *gd) {
    uint64_t v = gd->inode_table_lo;
    if (gd_has_hi(fs)) v |= (uint64_t)gd->inode_table_hi << 32;
    return v;
}

/* ------------------------------------------------------------------ */
/* Inode IO                                                            */
/* ------------------------------------------------------------------ */

static uint64_t inode_size_of(ext4_inode_t *in) {
    uint64_t hi = in->size_high;
    if (!EXT4_S_ISREG(in->mode)) hi = 0; /* size_high is i_dir_acl for dirs */
    return (uint64_t)in->size_lo | (hi << 32);
}

static void inode_set_size(ext4_inode_t *in, uint64_t size) {
    in->size_lo = (uint32_t)size;
    if (EXT4_S_ISREG(in->mode)) in->size_high = (uint32_t)(size >> 32);
}

static void inode_add_blocks(ext4_fs_t *fs, ext4_inode_t *in, int64_t fs_blocks) {
    uint64_t unit = (in->flags & EXT4_HUGE_FILE_FL)
                  ? 1u : (fs->block_size / 512u);
    int64_t delta = (int64_t)(fs_blocks * (int64_t)unit);
    int64_t cur = (int64_t)((uint64_t)in->blocks_lo |
                            ((uint64_t)in->blocks_high << 32));
    cur += delta;
    if (cur < 0) cur = 0;
    in->blocks_lo = (uint32_t)cur;
    in->blocks_high = (uint16_t)((uint64_t)cur >> 32);
}

static bool inode_load(ext4_fs_t *fs, uint64_t ino, ext4_inode_t *out) {
    if (ino == 0 || ino > fs->inodes_count) return false;
    uint32_t group = (uint32_t)((ino - 1) / fs->inodes_per_group);
    uint32_t index = (uint32_t)((ino - 1) % fs->inodes_per_group);
    if (group >= fs->groups) return false;
    uint64_t off = (uint64_t)gd_inode_table(fs, GD(fs, group)) * fs->block_size
                 + (uint64_t)index * fs->inode_size;
    if (!read_bytes(fs, off, fs->b_inode, fs->inode_size)) return false;
    memcpy(out, fs->b_inode, sizeof(*out));
    return true;
}

/* Write the whole cached inode image (b_inode) back to disk. */
static bool inode_store(ext4_fs_t *fs, uint64_t ino) {
    if (fs->read_only) return false;
    if (ino == 0 || ino > fs->inodes_count) return false;
    uint32_t group = (uint32_t)((ino - 1) / fs->inodes_per_group);
    uint32_t index = (uint32_t)((ino - 1) % fs->inodes_per_group);
    if (group >= fs->groups) return false;
    uint64_t off = (uint64_t)gd_inode_table(fs, GD(fs, group)) * fs->block_size
                 + (uint64_t)index * fs->inode_size;
    return write_bytes(fs, off, fs->b_inode, fs->inode_size);
}

static ext4_inode_t *inode_cached(ext4_fs_t *fs) {
    return (ext4_inode_t *)fs->b_inode;
}

/* ------------------------------------------------------------------ */
/* Bitmap helpers                                                      */
/* ------------------------------------------------------------------ */

static bool bit_get(const uint8_t *bm, uint32_t bit) {
    return (bm[bit / 8] >> (bit % 8)) & 1u;
}

static void bit_set(uint8_t *bm, uint32_t bit, bool value) {
    uint8_t mask = (uint8_t)(1u << (bit % 8));
    if (value) bm[bit / 8] |= mask;
    else bm[bit / 8] &= (uint8_t)~mask;
}

/* First clear bit at or after `start`, within `nbits`. -1 when full. */
static int64_t bit_find(const uint8_t *bm, uint32_t nbits, uint32_t start) {
    for (uint32_t bit = start; bit < nbits; bit++) {
        if (!bit_get(bm, bit)) return (int64_t)bit;
    }
    return -1;
}

static uint32_t group_first_block(ext4_fs_t *fs, uint32_t g) {
    return fs->first_data_block + g * fs->blocks_per_group;
}

static uint32_t group_block_count(ext4_fs_t *fs, uint32_t g) {
    uint64_t first = group_first_block(fs, g);
    if (first >= fs->blocks_count) return 0;
    uint64_t left = fs->blocks_count - first;
    return (uint32_t)(left > fs->blocks_per_group ? fs->blocks_per_group : left);
}

static uint32_t group_of_block(ext4_fs_t *fs, uint64_t blk) {
    if (blk < fs->first_data_block) return 0;
    return (uint32_t)((blk - fs->first_data_block) / fs->blocks_per_group);
}

/* ------------------------------------------------------------------ */
/* Allocation (defined in part 3, used by the mapping code)            */
/* ------------------------------------------------------------------ */

/* Carve one free block out of the group bitmap. 0 = none free.
 * The block is returned dirty: callers write it or zero it themselves. */
static uint32_t alloc_block(ext4_fs_t *fs, uint32_t prefer_group);
/* Return one block to the bitmap and bump the free counters. */
static void free_block(ext4_fs_t *fs, uint32_t blk);

/* ------------------------------------------------------------------ */
/* Extent tree                                                         */
/* ------------------------------------------------------------------ */

static uint32_t ext_len(const ext4_eent_t *e) {
    uint32_t len = e->ee_len;
    if (len > 32768u) len -= 32768u; /* high bit marks an unwritten extent */
    return len;
}

static bool ext_unwritten(const ext4_eent_t *e) { return e->ee_len > 32768u; }

static uint32_t ext_phys(const ext4_eent_t *e) {
    return ((uint32_t)e->ee_start_hi << 16) | e->ee_start_lo;
}

static void ext_set(ext4_eent_t *e, uint32_t logical, uint32_t len,
                    uint32_t phys) {
    e->ee_block = logical;
    e->ee_len = (uint16_t)len;
    e->ee_start_hi = (uint16_t)(phys >> 16);
    e->ee_start_lo = (uint32_t)phys;
}

static ext4_eh_t *node_hdr(void *node) { return (ext4_eh_t *)node; }

/* Descend to the leaf that would hold `logical`.
 * Returns false if the tree is malformed. On return the leaf is resident:
 * *leaf_blk == 0 means it lives inside the inode, otherwise fs->b_leaf holds
 * it. *phys is 0 for a hole, *idx is the insertion slot in that leaf. */
static bool extent_find(ext4_fs_t *fs, ext4_inode_t *in, uint32_t logical,
                        uint32_t *phys, uint32_t *leaf_blk, uint32_t *idx,
                        bool *unwritten) {
    *phys = 0;
    *leaf_blk = 0;
    *idx = 0;
    *unwritten = false;

    void *node = in->i_block;
    uint32_t leaf = 0;
    ext4_eh_t *h = node_hdr(node);
    if (h->eh_magic != EXT4_EXT_MAGIC) return false;

    for (int guard = 0; guard < 8; guard++) {
        if (h->eh_depth == 0) break;
        ext4_eidx_t *ix = (ext4_eidx_t *)(h + 1);
        int pick = -1;
        for (uint32_t i = 0; i < h->eh_entries; i++) {
            if (ix[i].ei_block <= logical) pick = (int)i;
        }
        if (pick < 0) return true; /* logical block precedes the whole tree */
        uint32_t child = ix[pick].ei_leaf_lo |
                       ((uint32_t)ix[pick].ei_leaf_hi << 16);
        if (child == 0) return true;
        if (!read_block(fs, child, fs->b_leaf)) return false;
        node = fs->b_leaf;
        leaf = child;
        h = node_hdr(node);
        if (h->eh_magic != EXT4_EXT_MAGIC) return false;
    }
    if (h->eh_depth != 0) return false;

    ext4_eent_t *e = (ext4_eent_t *)(h + 1);
    *leaf_blk = leaf;
    for (uint32_t i = 0; i < h->eh_entries; i++) {
        uint32_t start = e[i].ee_block;
        uint32_t len = ext_len(&e[i]);
        if (logical < start) {          /* hole before this extent */
            *idx = i;
            return true;
        }
        if (logical < start + len) {
            *phys = ext_phys(&e[i]) + (logical - start);
            *unwritten = ext_unwritten(&e[i]);
            *idx = i;
            return true;
        }
        *idx = i + 1;
    }
    return true;
}

/* Grow the inode-resident leaf (4 entries) into an external block so the
 * tree can hold hundreds of extents. Only valid at depth 0 and only when
 * the inode leaf is full. */
static bool extent_promote(ext4_fs_t *fs, ext4_inode_t *in) {
    ext4_eh_t *h = (ext4_eh_t *)in->i_block;
    if (h->eh_magic != EXT4_EXT_MAGIC || h->eh_depth != 0) return false;
    if (h->eh_entries < h->eh_max) return true;

    uint32_t nb = alloc_block(fs, 0);
    if (nb == 0) return false;

    uint32_t first_logical = 0;
    ext4_eent_t *old = (ext4_eent_t *)(h + 1);
    if (h->eh_entries > 0) first_logical = old[0].ee_block;

    memset(fs->b_data, 0, fs->block_size);
    memcpy(fs->b_data, in->i_block, 60); /* header + entries become the leaf */
    ext4_eh_t *lh = (ext4_eh_t *)fs->b_data;
    lh->eh_max = (uint16_t)((fs->block_size - sizeof(ext4_eh_t)) /
                            sizeof(ext4_eent_t));
    if (!write_block(fs, nb, fs->b_data)) {
        free_block(fs, nb);
        return false;
    }

    memset(in->i_block, 0, 60);
    ext4_eh_t *ih = (ext4_eh_t *)in->i_block;
    ih->eh_magic = EXT4_EXT_MAGIC;
    ih->eh_entries = 1;
    ih->eh_max = (uint16_t)((60 - sizeof(ext4_eh_t)) / sizeof(ext4_eidx_t));
    ih->eh_depth = 1;
    ext4_eidx_t *ix = (ext4_eidx_t *)(in->i_block + sizeof(ext4_eh_t));
    ix->ei_block = first_logical;
    ix->ei_leaf_lo = nb;
    ix->ei_leaf_hi = 0;

    inode_add_blocks(fs, in, 1);
    return true;
}

/* Insert a one-block extent at the slot found by extent_find(). */
static bool extent_insert(ext4_fs_t *fs, ext4_inode_t *in, uint32_t leaf_blk,
                          uint32_t idx, uint32_t logical, uint32_t phys) {
    ext4_eh_t *h;
    if (leaf_blk == 0) {
        h = (ext4_eh_t *)in->i_block;
    } else {
        if (!read_block(fs, leaf_blk, fs->b_leaf)) return false;
        h = (ext4_eh_t *)fs->b_leaf;
    }
    if (h->eh_magic != EXT4_EXT_MAGIC || h->eh_depth != 0) return false;
    if (h->eh_entries >= h->eh_max) return false;
    if (idx > h->eh_entries) idx = h->eh_entries;

    ext4_eent_t *e = (ext4_eent_t *)(h + 1);
    memmove(&e[idx + 1], &e[idx],
            (size_t)(h->eh_entries - idx) * sizeof(ext4_eent_t));
    ext_set(&e[idx], logical, 1, phys);
    h->eh_entries++;

    if (leaf_blk == 0) return true;
    return write_block(fs, leaf_blk, fs->b_leaf);
}

/* Walk every leaf entry, read-only. Used to free all mapped blocks. */
static bool extent_walk_read(ext4_fs_t *fs, ext4_inode_t *in, void *node,
                             uint32_t node_blk,
                             bool (*cb)(ext4_fs_t *, uint32_t, uint32_t, uint32_t, void *),
                             void *ctx) {
    ext4_eh_t *h = node_hdr(node);
    if (h->eh_magic != EXT4_EXT_MAGIC) return true;
    if (h->eh_depth > 0) {
        ext4_eidx_t *ix = (ext4_eidx_t *)(h + 1);
        for (uint32_t i = 0; i < h->eh_entries; i++) {
            uint32_t child = ix[i].ei_leaf_lo | ((uint32_t)ix[i].ei_leaf_hi << 16);
            if (child == 0) continue;
            if (!read_block(fs, child, fs->b_leaf)) return false;
            if (!extent_walk_read(fs, in, fs->b_leaf, child, cb, ctx)) return false;
            /* re-read this node: the child walk reused the scratch block */
            if (node_blk != 0) {
                if (!read_block(fs, node_blk, fs->b_leaf)) return false;
                node = fs->b_leaf;
                h = node_hdr(node);
            } else {
                /* root lives in the inode image; it was never overwritten */
            }
        }
        return true;
    }
    ext4_eent_t *e = (ext4_eent_t *)(h + 1);
    for (uint32_t i = 0; i < h->eh_entries; i++) {
        if (!cb(fs, ext_phys(&e[i]), ext_len(&e[i]), e[i].ee_block, ctx)) return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Block-mapped (ext2-style) trees                                     */
/* ------------------------------------------------------------------ */

static void blockmap_coord(uint32_t logical, uint32_t per, int *level,
                           uint32_t *index) {
    const uint32_t direct = 12;
    if (logical < direct) { *level = 0; *index = logical; return; }
    uint32_t rest = logical - direct;
    if (rest < per) { *level = 1; *index = rest; return; }
    rest -= per;
    uint64_t l2 = (uint64_t)per * per;
    if (rest < l2) { *level = 2; *index = rest; return; }
    rest -= (uint32_t)l2;
    *level = 3;
    *index = rest;
}

/* Resolve logical → physical for a non-extent inode. Allocates the data
 * block and any missing indirect block when `alloc` is set. */
static uint32_t blockmap_map(ext4_fs_t *fs, ext4_inode_t *in, uint32_t logical,
                             bool alloc, bool *fresh) {
    int level;
    uint32_t index;
    blockmap_coord(logical, fs->block_size / 4u, &level, &index);

    if (level == 0) {
        uint32_t *slot = (uint32_t *)(in->i_block) + index;
        if (*slot) return *slot;
        if (!alloc) return 0;
        uint32_t nb = alloc_block(fs, 0);
        if (!nb) return 0;
        *slot = nb;
        inode_add_blocks(fs, in, 1);
        if (fresh) *fresh = true;
        return nb;
    }

    /* Descend through indirect blocks, re-reading each parent as we go. */
    uint32_t node = ((uint32_t *)in->i_block)[11 + level];
    for (int l = level; l > 0; l--) {
        uint32_t step = 1;
        for (int s = 1; s < l; s++) step *= fs->block_size / 4u;
        uint32_t slot_i = index / step;
        uint32_t rem = index % step;

        if (node == 0) {
            if (!alloc) return 0;
            node = alloc_block(fs, 0);
            if (node == 0) return 0;
            memset(fs->b_data, 0, fs->block_size);
            if (!write_block(fs, node, fs->b_data)) { free_block(fs, node); return 0; }
            ((uint32_t *)in->i_block)[11 + l] = node;
            inode_add_blocks(fs, in, 1);
        }
        if (!read_block(fs, node, fs->b_leaf)) return 0;
        uint32_t *slots = (uint32_t *)fs->b_leaf;
        if (l == 1) {
            if (slots[slot_i] == 0) {
                if (!alloc) return 0;
                uint32_t nb = alloc_block(fs, 0);
                if (nb == 0) return 0;
                slots[slot_i] = nb;
                if (!write_block(fs, node, fs->b_leaf)) { free_block(fs, nb); return 0; }
                inode_add_blocks(fs, in, 1);
                if (fresh) *fresh = true;
                return nb;
            }
            if (fresh) *fresh = false;
            return slots[slot_i];
        }
        uint32_t child = slots[slot_i];
        if (child == 0) {
            if (!alloc) return 0;
            child = alloc_block(fs, 0);
            if (child == 0) return 0;
            memset(fs->b_data, 0, fs->block_size);
            if (!write_block(fs, child, fs->b_data)) { free_block(fs, child); return 0; }
            slots[slot_i] = child;
            if (!write_block(fs, node, fs->b_leaf)) { free_block(fs, child); return 0; }
            inode_add_blocks(fs, in, 1);
        }
        index = rem;
        node = child;
    }
    return 0;
}

/* Resolve logical → physical, allocating when asked. 0 = failure/hole. */
static uint32_t map_block(ext4_fs_t *fs, uint64_t ino, ext4_inode_t *in,
                          uint32_t logical, bool alloc, bool *fresh) {
    (void)ino;
    if (fresh) *fresh = false;

    if (in->flags & EXT4_EXTENTS_FL) {
        uint32_t phys = 0, leaf = 0, idx = 0;
        bool unwritten = false;
        if (!extent_find(fs, in, logical, &phys, &leaf, &idx, &unwritten)) return 0;
        if (phys != 0) return phys;
        if (!alloc) return 0;

        ext4_eh_t *h = (ext4_eh_t *)in->i_block;
        if (leaf == 0 && h->eh_entries >= h->eh_max) {
            if (!extent_promote(fs, in)) return 0;
            if (!extent_find(fs, in, logical, &phys, &leaf, &idx, &unwritten)) return 0;
            if (phys != 0) return phys;
        }
        uint32_t hint = (in->i_block[0] | in->i_block[1] | in->i_block[2] |
                         in->i_block[3]) ? 0 : 0;
        (void)hint;
        uint32_t nb = alloc_block(fs, 0);
        if (nb == 0) return 0;
        if (!extent_insert(fs, in, leaf, idx, logical, nb)) {
            free_block(fs, nb);
            return 0;
        }
        inode_add_blocks(fs, in, 1);
        if (fresh) *fresh = true;
        return nb;
    }
    return blockmap_map(fs, in, logical, alloc, fresh);
}

/* ------------------------------------------------------------------ */
/* Freeing blocks                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t keep_logical;  /* first logical block that must survive */
    uint32_t freed;
} free_ctx_t;

static bool free_extent_cb(ext4_fs_t *fs, uint32_t phys, uint32_t len,
                           uint32_t logical, void *ctx) {
    free_ctx_t *fc = ctx;
    for (uint32_t i = 0; i < len; i++) {
        uint64_t lg = (uint64_t)logical + i;
        if (lg < fc->keep_logical) continue;
        if (phys + i == 0) continue;
        free_block(fs, phys + i);
        fc->freed++;
    }
    return true;
}

/* Free every data block of a block-mapped inode (indirect blocks too). */
static bool blockmap_free(ext4_fs_t *fs, ext4_inode_t *in, uint32_t logical,
                          int level, uint32_t node, free_ctx_t *fc) {
    if (level == 0) {
        if (logical >= fc->keep_logical && node != 0) {
            free_block(fs, node);
            fc->freed++;
        }
        return true;
    }
    if (node == 0) return true;
    if (!read_block(fs, node, fs->b_leaf)) return false;
    uint32_t *slots = (uint32_t *)fs->b_leaf;
    uint32_t per = fs->block_size / 4u;
    uint32_t span = 1;
    for (int s = 1; s < level; s++) span *= per;
    for (uint32_t i = 0; i < per; i++) {
        if (slots[i] == 0) continue;
        if (!blockmap_free(fs, in, logical + i * span, level - 1, slots[i], fc))
            return false;
        /* the child walk reused the scratch block: reload this one */
        if (!read_block(fs, node, fs->b_leaf)) return false;
    }
    if (fc->keep_logical <= logical) {
        free_block(fs, node);
        fc->freed++;
    }
    return true;
}

/* Free every data block of the inode beyond `keep_bytes`. keep_bytes == 0
 * dismantles the whole tree. Partial truncation is only implemented for
 * inode-resident extent trees (depth 0), which is what this driver creates;
 * ponytail: support trimming deeper trees if a huge file ever needs it. */
static bool inode_free_data(ext4_fs_t *fs, ext4_inode_t *in,
                            uint64_t keep_bytes) {
    free_ctx_t fc = { .keep_logical = (keep_bytes + fs->block_size - 1) /
                                      fs->block_size, .freed = 0 };

    if (in->flags & EXT4_EXTENTS_FL) {
        ext4_eh_t *h = (ext4_eh_t *)in->i_block;
        if (h->eh_magic != EXT4_EXT_MAGIC) return true;
        if (keep_bytes != 0 && h->eh_depth != 0) return false;

        if (keep_bytes == 0) {
            if (!extent_walk_read(fs, in, in->i_block, 0, free_extent_cb, &fc))
                return false;
            h->eh_entries = 0;
        } else {
            /* Free everything past the keep point, then compact the leaf. */
            uint32_t keep_blocks = (uint32_t)fc.keep_logical;
            ext4_eent_t *e = (ext4_eent_t *)(h + 1);
            uint32_t n = h->eh_entries;
            uint32_t out = 0;
            for (uint32_t i = 0; i < n; i++) {
                uint32_t start = e[i].ee_block;
                uint32_t len = ext_len(&e[i]);
                if (start >= keep_blocks) {
                    for (uint32_t k = 0; k < len; k++) {
                        free_block(fs, ext_phys(&e[i]) + k);
                        fc.freed++;
                    }
                    continue;
                }
                if (start + len > keep_blocks) {
                    uint32_t keep_here = keep_blocks - start;
                    uint32_t phys = ext_phys(&e[i]);
                    for (uint32_t k = keep_here; k < len; k++) {
                        free_block(fs, phys + k);
                        fc.freed++;
                    }
                    if (ext_unwritten(&e[i])) {
                        e[i].ee_len = (uint16_t)(keep_here + 32768u);
                    } else {
                        e[i].ee_len = (uint16_t)keep_here;
                    }
                }
                e[out++] = e[i];
            }
            h->eh_entries = (uint16_t)out;
        }
    } else {
        uint32_t *ib = (uint32_t *)in->i_block;
        uint32_t per = fs->block_size / 4u;
        uint32_t keep_blocks = (uint32_t)fc.keep_logical;

        for (uint32_t i = 0; i < 12; i++) {
            if (ib[i] == 0) continue;
            if (i >= keep_blocks) {
                free_block(fs, ib[i]);
                fc.freed++;
                ib[i] = 0;
            }
        }
        /* Base logical block of each indirect tree. */
        uint32_t base = 12;
        for (int level = 1; level <= 3; level++) {
            if (ib[11 + level] != 0) {
                /* blockmap_free releases the node itself when every block
                 * it covers is beyond the keep point. */
                if (!blockmap_free(fs, in, base, level, ib[11 + level], &fc))
                    return false;
                if (base >= keep_blocks) ib[11 + level] = 0;
            }
            base += (level == 1) ? per : (level == 2 ? per * per : per * per * per);
        }
    }

    if (fc.freed) inode_add_blocks(fs, in, -(int64_t)fc.freed);
    return true;
}

/* ------------------------------------------------------------------ */
/* Block and inode allocation                                          */
/* ------------------------------------------------------------------ */

static void gd_dec_free_blocks(ext4_fs_t *fs, ext4_gd_t *gd) {
    if (gd->free_blocks_count_lo > 0) {
        gd->free_blocks_count_lo--;
    } else if (fs->desc_size >= 64 && gd->free_blocks_count_hi > 0) {
        gd->free_blocks_count_hi--;
        gd->free_blocks_count_lo = 0xFFFFu;
    }
}

static void gd_inc_free_blocks(ext4_fs_t *fs, ext4_gd_t *gd) {
    if (gd->free_blocks_count_lo == 0xFFFFu) {
        if (fs->desc_size >= 64) gd->free_blocks_count_hi++;
        else return; /* 32-bit descriptor: counter saturated */
    }
    gd->free_blocks_count_lo++;
}

static void gd_dec_free_inodes(ext4_fs_t *fs, ext4_gd_t *gd) {
    if (gd->free_inodes_count_lo > 0) {
        gd->free_inodes_count_lo--;
    } else if (fs->desc_size >= 64 && gd->free_inodes_count_hi > 0) {
        gd->free_inodes_count_hi--;
        gd->free_inodes_count_lo = 0xFFFFu;
    }
}

static void gd_inc_free_inodes(ext4_fs_t *fs, ext4_gd_t *gd) {
    if (gd->free_inodes_count_lo == 0xFFFFu) {
        if (fs->desc_size >= 64) gd->free_inodes_count_hi++;
        else return;
    }
    gd->free_inodes_count_lo++;
}

static uint32_t alloc_block(ext4_fs_t *fs, uint32_t prefer_group) {
    if (fs->read_only || fs->groups == 0) return 0;
    if (prefer_group >= fs->groups) prefer_group = 0;

    for (uint32_t pass = 0; pass < 2; pass++) {
        for (uint32_t i = 0; i < fs->groups; i++) {
            uint32_t g = (pass == 0) ? (prefer_group + i) % fs->groups : i;
            ext4_gd_t *gd = GD(fs, g);
            if (gd->free_blocks_count_lo == 0 &&
                !(fs->desc_size >= 64 && gd->free_blocks_count_hi != 0)) {
                continue;
            }
            if (gd->flags & EXT4_BG_BLOCK_UNINIT) continue;
            uint64_t bm = gd_block_bitmap(fs, gd);
            if (bm == 0 || !read_block(fs, (uint32_t)bm, fs->b_bitmap)) continue;

            uint32_t nbits = group_block_count(fs, g);
            uint32_t cap = (uint32_t)(fs->block_size * 8u);
            if (nbits > cap) nbits = cap;
            int64_t bit = bit_find(fs->b_bitmap, nbits, 0);
            if (bit < 0) continue;

            uint64_t blk = (uint64_t)group_first_block(fs, g) + (uint64_t)bit;
            if (blk < fs->first_data_block || blk >= fs->blocks_count) continue;

            bit_set(fs->b_bitmap, (uint32_t)bit, true);
            if (!write_block(fs, (uint32_t)bm, fs->b_bitmap)) continue;

            gd_dec_free_blocks(fs, gd);
            if (sb_free_blocks(fs) > 0) {
                sb_set_free_blocks(fs, sb_free_blocks(fs) - 1);
            }
            sb_flush(fs);
            gd_flush(fs, g);
            return (uint32_t)blk;
        }
    }
    return 0;
}

static void free_block(ext4_fs_t *fs, uint32_t blk) {
    if (fs->read_only || blk == 0 || blk >= fs->blocks_count) return;
    if (blk < fs->first_data_block) return;

    uint32_t g = group_of_block(fs, blk);
    if (g >= fs->groups) return;
    ext4_gd_t *gd = GD(fs, g);
    uint32_t bit = (uint32_t)(blk - (uint64_t)group_first_block(fs, g));
    uint64_t bm = gd_block_bitmap(fs, gd);
    if (bm == 0 || !read_block(fs, (uint32_t)bm, fs->b_bitmap)) return;
    if (!bit_get(fs->b_bitmap, bit)) return; /* already free: ignore */

    bit_set(fs->b_bitmap, bit, false);
    if (!write_block(fs, (uint32_t)bm, fs->b_bitmap)) return;

    gd_inc_free_blocks(fs, gd);
    sb_set_free_blocks(fs, sb_free_blocks(fs) + 1);
    sb_flush(fs);
    gd_flush(fs, g);
}

/* Carve out a free inode. `prefer_group` biases placement for locality. */
static uint64_t alloc_inode(ext4_fs_t *fs, uint32_t prefer_group) {
    if (fs->read_only || fs->groups == 0) return 0;
    if (prefer_group >= fs->groups) prefer_group = 0;

    for (uint32_t pass = 0; pass < 2; pass++) {
        for (uint32_t i = 0; i < fs->groups; i++) {
            uint32_t g = (pass == 0) ? (prefer_group + i) % fs->groups : i;
            ext4_gd_t *gd = GD(fs, g);
            if (gd->free_inodes_count_lo == 0 &&
                !(fs->desc_size >= 64 && gd->free_inodes_count_hi != 0)) {
                continue;
            }
            if (gd->flags & EXT4_BG_INODE_UNINIT) continue;
            uint64_t bm = gd_inode_bitmap(fs, gd);
            if (bm == 0 || !read_block(fs, (uint32_t)bm, fs->b_bitmap)) continue;

            uint32_t nbits = fs->inodes_per_group;
            uint32_t cap = (uint32_t)(fs->block_size * 8u);
            if (nbits > cap) nbits = cap;
            uint32_t start = (g == 0) ? (fs->first_ino - 1u) : 0u;
            int64_t bit = bit_find(fs->b_bitmap, nbits, start);
            if (bit < 0) continue;

            uint64_t ino = (uint64_t)g * fs->inodes_per_group + (uint64_t)bit + 1;
            if (ino == 0 || ino > fs->inodes_count) continue;

            bit_set(fs->b_bitmap, (uint32_t)bit, true);
            if (!write_block(fs, (uint32_t)bm, fs->b_bitmap)) continue;

            gd_dec_free_inodes(fs, gd);
            SB(fs)->free_inodes_count--;
            sb_flush(fs);
            gd_flush(fs, g);
            return ino;
        }
    }
    return 0;
}

static void free_inode_number(ext4_fs_t *fs, uint64_t ino) {
    if (fs->read_only || ino == 0 || ino > fs->inodes_count) return;
    uint32_t g = (uint32_t)((ino - 1) / fs->inodes_per_group);
    if (g >= fs->groups) return;
    ext4_gd_t *gd = GD(fs, g);
    uint32_t bit = (uint32_t)((ino - 1) % fs->inodes_per_group);
    uint64_t bm = gd_inode_bitmap(fs, gd);
    if (bm == 0 || !read_block(fs, (uint32_t)bm, fs->b_bitmap)) return;
    if (!bit_get(fs->b_bitmap, bit)) return;

    bit_set(fs->b_bitmap, bit, false);
    if (!write_block(fs, (uint32_t)bm, fs->b_bitmap)) return;

    gd_inc_free_inodes(fs, gd);
    SB(fs)->free_inodes_count++;
    sb_flush(fs);
    gd_flush(fs, g);
}

/* ------------------------------------------------------------------ */
/* Directories                                                         */
/* ------------------------------------------------------------------ */

typedef bool (*dent_cb_t)(ext4_fs_t *fs, ext4_dent_t *d, void *ctx);

/* Walk every entry of a directory, unused slots included. */
static bool dir_walk(ext4_fs_t *fs, uint64_t dir_ino, ext4_inode_t *in,
                     dent_cb_t cb, void *ctx) {
    uint64_t size = inode_size_of(in);
    uint32_t bs = fs->block_size;
    uint32_t blocks = (uint32_t)((size + bs - 1) / bs);

    for (uint32_t logical = 0; logical < blocks; logical++) {
        bool fresh = false;
        uint32_t phys = map_block(fs, dir_ino, in, logical, false, &fresh);
        if (phys == 0) continue;
        if (!read_block(fs, phys, fs->b_data)) return false;
        uint8_t *blk = fs->b_data;

        for (uint32_t off = 0; off + 8 <= bs; ) {
            uint32_t rec = *(uint16_t *)(blk + off + 4);
            uint32_t ino = *(uint32_t *)(blk + off);
            uint8_t nlen = blk[off + 6];
            if (rec < 8 || rec % 4 != 0 || off + rec > bs) break;
            if (ino != 0 && nlen > 0) {
                ext4_dent_t d;
                memset(&d, 0, sizeof(d));
                d.logical = logical;
                d.offset = off;
                d.rec_len = rec;
                d.inode = ino;
                d.name_len = nlen;
                d.ftype = blk[off + 7];
                memcpy(d.name, blk + off + 8, nlen);
                d.name[nlen] = '\0';
                if (!cb(fs, &d, ctx)) return true;
            }
            off += rec;
        }
    }
    return true;
}

/* Patch one directory entry into `phys` at `offset`. */
static bool dir_put_entry(ext4_fs_t *fs, uint32_t phys, uint32_t offset,
                          uint32_t rec_len, uint64_t ino, const char *name,
                          uint8_t nlen, uint8_t ftype) {
    if (!read_block(fs, phys, fs->b_data)) return false;
    uint8_t *blk = fs->b_data;
    memset(blk + offset, 0, rec_len);
    *(uint32_t *)(blk + offset) = (uint32_t)ino;
    *(uint16_t *)(blk + offset + 4) = (uint16_t)rec_len;
    blk[offset + 6] = nlen;
    blk[offset + 7] = ftype;
    memcpy(blk + offset + 8, name, nlen);
    return write_block(fs, phys, fs->b_data);
}

typedef struct {
    const char *name;
    uint8_t nlen;
    bool found;
    uint64_t ino;
    uint8_t ftype;
    ext4_dent_t slot;      /* best free/splittable slot, if any */
    bool have_slot;
} find_ctx_t;

static bool find_name_cb(ext4_fs_t *fs, ext4_dent_t *d, void *vctx) {
    (void)fs;
    find_ctx_t *c = vctx;
    if (d->name_len == c->nlen && memcmp(d->name, c->name, c->nlen) == 0) {
        c->found = true;
        c->ino = d->inode;
        c->ftype = d->ftype;
        return false;
    }
    uint32_t used = 8u + ((d->name_len + 3u) & ~3u);
    uint32_t need = 8u + ((c->nlen + 3u) & ~3u);
    if (!c->have_slot) {
        if (d->inode == 0 && d->rec_len >= need) {
            c->slot = *d;
            c->have_slot = true;
        } else if (d->inode != 0 && d->rec_len >= used + need) {
            c->slot = *d;
            c->slot.rec_len = used;  /* split: keep this entry, new one after */
            c->have_slot = true;
        }
    }
    return true;
}

static bool name_valid(const char *name, uint8_t *nlen_out) {
    size_t n = 0;
    if (name == NULL) return false;
    while (name[n] != '\0') {
        if (name[n] == '/' || n >= EXT4_NAME_MAX) return false;
        n++;
    }
    if (n == 0) return false;
    *nlen_out = (uint8_t)n;
    return true;
}

/* Insert `name` into the directory. Grows the directory by one block when
 * no entry has room. */
static bool dir_add_entry(ext4_fs_t *fs, uint64_t dir_ino, const char *name,
                          uint64_t ino, uint8_t ftype) {
    uint8_t nlen;
    if (!name_valid(name, &nlen)) return false;

    ext4_inode_t probe;
    if (!inode_load(fs, dir_ino, &probe)) return false;
    ext4_inode_t *dir = inode_cached(fs); /* map_block edits land here too */

    find_ctx_t c;
    memset(&c, 0, sizeof(c));
    c.name = name;
    c.nlen = nlen;
    dir_walk(fs, dir_ino, dir, find_name_cb, &c);
    if (c.found) return false;

    uint32_t need = 8u + ((nlen + 3u) & ~3u);
    uint32_t bs = fs->block_size;

    if (c.have_slot) {
        /* Slot lives in a block we must re-read and patch. */
        bool fresh = false;
        uint32_t phys = map_block(fs, dir_ino, dir, c.slot.logical, false, &fresh);
        if (phys == 0) return false;
        if (c.slot.inode == 0) {
            uint32_t old = c.slot.rec_len;
            uint32_t rec = (old - need >= 8u) ? need : old;
            if (!dir_put_entry(fs, phys, c.slot.offset, rec, ino, name, nlen, ftype))
                return false;
            if (rec != old) {
                /* leave the remainder as a fresh free slot */
                if (!read_block(fs, phys, fs->b_data)) return false;
                uint8_t *blk = fs->b_data;
                uint32_t off = c.slot.offset + rec;
                *(uint32_t *)(blk + off) = 0;
                *(uint16_t *)(blk + off + 4) = (uint16_t)(old - rec);
                blk[off + 6] = 0;
                blk[off + 7] = 0;
                if (!write_block(fs, phys, fs->b_data)) return false;
            }
            return true;
        }
        /* Occupied entry with room to split. */
        if (!read_block(fs, phys, fs->b_data)) return false;
        uint8_t *blk = fs->b_data;
        uint32_t off = c.slot.offset;
        uint32_t old = *(uint16_t *)(blk + off + 4);
        *(uint16_t *)(blk + off + 4) = (uint16_t)c.slot.rec_len; /* shrunk */
        uint32_t tail = off + c.slot.rec_len;
        memset(blk + tail, 0, old - c.slot.rec_len);
        *(uint32_t *)(blk + tail) = (uint32_t)ino;
        *(uint16_t *)(blk + tail + 4) = (uint16_t)(old - c.slot.rec_len);
        blk[tail + 6] = nlen;
        blk[tail + 7] = ftype;
        memcpy(blk + tail + 8, name, nlen);
        return write_block(fs, phys, fs->b_data);
    }

    /* No room: grow the directory by one block and use it. */
    uint64_t size = inode_size_of(dir);
    uint32_t logical = (uint32_t)(size / bs);
    bool fresh = false;
    uint32_t phys = map_block(fs, dir_ino, dir, logical, true, &fresh);
    if (phys == 0) return false;
    /* dir_put_entry memsets the whole record, which for a record spanning
     * the fresh block means the block is fully initialised. */
    if (!dir_put_entry(fs, phys, 0, bs, ino, name, nlen, ftype)) return false;

    inode_set_size(dir, size + bs);
    return inode_store(fs, dir_ino);
}

typedef struct {
    const char *name;
    uint8_t nlen;
    bool found;
    uint32_t logical;
    uint32_t offset;
    uint32_t rec_len;
    uint64_t ino;
    uint8_t ftype;
} rm_ctx_t;

/* Remove `name` from the directory, merging its record length backwards. */
static bool dir_remove_entry(ext4_fs_t *fs, uint64_t dir_ino, const char *name,
                             uint64_t *out_ino, uint8_t *out_ftype) {
    uint8_t nlen;
    if (!name_valid(name, &nlen)) return false;

    ext4_inode_t dir;
    if (!inode_load(fs, dir_ino, &dir)) return false;

    uint32_t bs = fs->block_size;
    uint64_t size = inode_size_of(&dir);
    uint32_t blocks = (uint32_t)((size + bs - 1) / bs);

    for (uint32_t logical = 0; logical < blocks; logical++) {
        bool fresh = false;
        uint32_t phys = map_block(fs, dir_ino, &dir, logical, false, &fresh);
        if (phys == 0) continue;
        if (!read_block(fs, phys, fs->b_data)) return false;
        uint8_t *blk = fs->b_data;

        for (uint32_t off = 0; off + 8 <= bs; ) {
            uint32_t rec = *(uint16_t *)(blk + off + 4);
            uint32_t ino = *(uint32_t *)(blk + off);
            uint8_t nl = blk[off + 6];
            if (rec < 8 || rec % 4 != 0 || off + rec > bs) break;
            if (ino != 0 && nl == nlen && memcmp(blk + off + 8, name, nlen) == 0) {
                if (out_ino) *out_ino = ino;
                if (out_ftype) *out_ftype = blk[off + 7];
                if (off == 0) {
                    /* First entry in the block: mark the slot unused. */
                    *(uint32_t *)(blk + off) = 0;
                    blk[off + 6] = 0;
                    blk[off + 7] = 0;
                } else {
                    /* Merge into the entry that precedes this one. */
                    uint32_t p = 0;
                    bool have_prev = false;
                    uint32_t scan = 0;
                    while (scan + 8 <= off) {
                        uint32_t r = *(uint16_t *)(blk + scan + 4);
                        if (r < 8 || scan + r != off) break;
                        p = scan;
                        have_prev = true;
                        scan += r;
                    }
                    if (have_prev) {
                        *(uint16_t *)(blk + p + 4) =
                            (uint16_t)(*(uint16_t *)(blk + p + 4) + rec);
                    } else {
                        *(uint32_t *)(blk + off) = 0;
                    }
                }
                return write_block(fs, phys, fs->b_data);
            }
            off += rec;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Namespace operations                                                */
/* ------------------------------------------------------------------ */

static uint32_t group_of_inode(ext4_fs_t *fs, uint64_t ino) {
    if (ino == 0) return 0;
    return (uint32_t)((ino - 1) / fs->inodes_per_group);
}

static uint8_t ftype_of_mode(uint16_t mode) {
    if (EXT4_S_ISDIR(mode)) return EXT4_FT_DIR;
    if (EXT4_S_ISLNK(mode)) return EXT4_FT_SYMLINK;
    return EXT4_FT_REG_FILE;
}

/* Lay out a fresh inode image in b_inode. */
static void inode_prepare_new(ext4_fs_t *fs, uint16_t mode) {
    ext4_inode_t *in;
    memset(fs->b_inode, 0, fs->inode_size);
    in = inode_cached(fs);
    in->mode = mode;
    in->links = EXT4_S_ISDIR(mode) ? 2u : 1u;
    in->atime = in->ctime = in->mtime = now_secs();
    if (fs->has_extents) {
        in->flags = EXT4_EXTENTS_FL;
        ext4_eh_t *h = (ext4_eh_t *)in->i_block;
        h->eh_magic = EXT4_EXT_MAGIC;
        h->eh_entries = 0;
        h->eh_max = (uint16_t)((60 - sizeof(ext4_eh_t)) / sizeof(ext4_eent_t));
        h->eh_depth = 0;
    }
    if (fs->inode_size > 128) {
        uint16_t extra = fs->sb->want_extra_isize ? fs->sb->want_extra_isize
                                                  : fs->sb->min_extra_isize;
        *(uint16_t *)((uint8_t *)in + 128) = extra;
    }
}

typedef struct {
    const char *name;
    uint8_t nlen;
    ext4_stat_t st;
    bool found;
} lookup_ctx_t;

static bool lookup_cb(ext4_fs_t *fs, ext4_dent_t *d, void *vctx) {
    lookup_ctx_t *c = vctx;
    if (d->name_len != c->nlen || memcmp(d->name, c->name, c->nlen) != 0) {
        return true;
    }
    c->found = ext4_stat(fs, d->inode, &c->st);
    return false;
}

typedef struct {
    uint64_t except_ino;
    uint64_t entries;
} empty_ctx_t;

static bool empty_cb(ext4_fs_t *fs, ext4_dent_t *d, void *vctx) {
    (void)fs;
    empty_ctx_t *c = vctx;
    if (d->name_len == 1 && d->name[0] == '.') return true;
    if (d->name_len == 2 && d->name[0] == '.' && d->name[1] == '.') return true;
    c->entries++;
    return true;
}

static bool dir_is_empty(ext4_fs_t *fs, uint64_t dir_ino) {
    ext4_inode_t dir;
    if (!inode_load(fs, dir_ino, &dir)) return false;
    empty_ctx_t c = { .except_ino = dir_ino, .entries = 0 };
    dir_walk(fs, dir_ino, &dir, empty_cb, &c);
    return c.entries == 0;
}

bool ext4_create(ext4_fs_t *fs, uint64_t dir_ino, const char *name,
                 uint16_t mode, uint64_t *out_ino) {
    uint8_t nlen;
    if (fs == NULL || fs->read_only || !name_valid(name, &nlen)) return false;

    ext4_stat_t existing;
    if (ext4_lookup(fs, dir_ino, name, &existing)) return false;

    ext4_inode_t parent;
    if (!inode_load(fs, dir_ino, &parent)) return false;
    if (!EXT4_S_ISDIR(parent.mode)) return false;

    uint64_t ino = alloc_inode(fs, group_of_inode(fs, dir_ino));
    if (ino == 0) return false;

    inode_prepare_new(fs, mode);
    if (!inode_store(fs, ino)) {
        free_inode_number(fs, ino);
        return false;
    }
    if (!dir_add_entry(fs, dir_ino, name, ino, ftype_of_mode(mode))) {
        free_inode_number(fs, ino);
        return false;
    }
    if (out_ino) *out_ino = ino;
    return true;
}

bool ext4_mkdir(ext4_fs_t *fs, uint64_t dir_ino, const char *name,
                uint16_t mode, uint64_t *out_ino) {
    uint8_t nlen;
    if (fs == NULL || fs->read_only || !name_valid(name, &nlen)) return false;
    if (!EXT4_S_ISDIR(mode)) mode = (uint16_t)(EXT4_S_IFDIR | (mode & 07777));

    ext4_stat_t existing;
    if (ext4_lookup(fs, dir_ino, name, &existing)) return false;

    ext4_inode_t parent;
    if (!inode_load(fs, dir_ino, &parent)) return false;
    if (!EXT4_S_ISDIR(parent.mode)) return false;

    uint32_t pg = group_of_inode(fs, dir_ino);
    uint64_t ino = alloc_inode(fs, pg);
    if (ino == 0) return false;
    uint32_t blk = alloc_block(fs, pg);
    if (blk == 0) {
        free_inode_number(fs, ino);
        return false;
    }

    /* First directory block: "." and "..". */
    uint32_t bs = fs->block_size;
    memset(fs->b_data, 0, bs);
    {
        uint8_t *b = fs->b_data;
        *(uint32_t *)(b) = (uint32_t)ino;
        *(uint16_t *)(b + 4) = 12;
        b[6] = 1;
        b[7] = EXT4_FT_DIR;
        b[8] = '.';
        *(uint32_t *)(b + 12) = (uint32_t)dir_ino;
        *(uint16_t *)(b + 16) = (uint16_t)(bs - 12);
        b[18] = 2;
        b[19] = EXT4_FT_DIR;
        b[20] = '.';
        b[21] = '.';
    }
    if (!write_block(fs, blk, fs->b_data)) {
        free_block(fs, blk);
        free_inode_number(fs, ino);
        return false;
    }

    inode_prepare_new(fs, mode);
    {
        ext4_inode_t *in = inode_cached(fs);
        ext4_eh_t *h = (ext4_eh_t *)in->i_block;
        h->eh_entries = 1;
        ext_set((ext4_eent_t *)(h + 1), 0, 1, blk);
        inode_set_size(in, bs);
        inode_add_blocks(fs, in, 1);
    }
    if (!inode_store(fs, ino)) {
        free_block(fs, blk);
        free_inode_number(fs, ino);
        return false;
    }

    if (!dir_add_entry(fs, dir_ino, name, ino, EXT4_FT_DIR)) {
        free_block(fs, blk);
        free_inode_number(fs, ino);
        return false;
    }

    /* The parent gains one subdirectory link. */
    {
        ext4_inode_t tmp;
        if (inode_load(fs, dir_ino, &tmp)) {
            ext4_inode_t *p = inode_cached(fs);
            p->links++;
            p->mtime = p->ctime = now_secs();
            inode_store(fs, dir_ino);
        }
    }
    {
        uint32_t g = group_of_inode(fs, dir_ino);
        if (g < fs->groups) {
            GD(fs, g)->used_dirs_count_lo++;
            gd_flush(fs, g);
        }
    }
    if (out_ino) *out_ino = ino;
    return true;
}

bool ext4_unlink(ext4_fs_t *fs, uint64_t dir_ino, const char *name,
                 bool as_dir) {
    uint8_t nlen;
    if (fs == NULL || fs->read_only || !name_valid(name, &nlen)) return false;

    ext4_stat_t st;
    if (!ext4_lookup(fs, dir_ino, name, &st)) return false;
    if (as_dir && !EXT4_S_ISDIR(st.mode)) return false;
    if (!as_dir && EXT4_S_ISDIR(st.mode)) return false;
    if (as_dir && !dir_is_empty(fs, st.ino)) return false;

    uint64_t victim = st.ino;
    uint8_t ftype;
    if (!dir_remove_entry(fs, dir_ino, name, &victim, &ftype)) return false;

    ext4_inode_t dead;
    if (!inode_load(fs, victim, &dead)) return false;
    if (!inode_free_data(fs, &dead, 0)) return false;

    memset(fs->b_inode, 0, fs->inode_size);
    {
        ext4_inode_t *in = inode_cached(fs);
        in->dtime = now_secs();
        in->links = 0;
    }
    inode_store(fs, victim);
    free_inode_number(fs, victim);

    if (as_dir) {
        ext4_inode_t tmp;
        if (inode_load(fs, dir_ino, &tmp)) {
            ext4_inode_t *p = inode_cached(fs);
            if (p->links > 0) p->links--;
            p->mtime = p->ctime = now_secs();
            inode_store(fs, dir_ino);
        }
        uint32_t g = group_of_inode(fs, dir_ino);
        if (g < fs->groups && GD(fs, g)->used_dirs_count_lo > 0) {
            GD(fs, g)->used_dirs_count_lo--;
            gd_flush(fs, g);
        }
    }
    return true;
}

bool ext4_truncate(ext4_fs_t *fs, uint64_t ino, uint64_t size) {
    if (fs == NULL || fs->read_only) return false;
    ext4_inode_t probe;
    if (!inode_load(fs, ino, &probe)) return false;
    ext4_inode_t *in = inode_cached(fs);

    uint64_t cur = inode_size_of(in);
    if (size < cur && !inode_free_data(fs, in, size)) return false;
    inode_set_size(in, size);
    in->mtime = in->ctime = now_secs();
    return inode_store(fs, ino);
}

/* ------------------------------------------------------------------ */
/* File data                                                           */
/* ------------------------------------------------------------------ */

uint64_t ext4_read(ext4_fs_t *fs, uint64_t ino, uint64_t offset, void *buf,
                   uint64_t len) {
    if (fs == NULL || buf == NULL || len == 0) return 0;
    ext4_inode_t probe;
    if (!inode_load(fs, ino, &probe)) return 0;
    ext4_inode_t *in = inode_cached(fs);

    uint64_t size = inode_size_of(in);
    if (offset >= size) return 0;
    if (len > size - offset) len = size - offset;

    uint32_t bs = fs->block_size;
    uint8_t *p = buf;
    uint64_t done = 0;
    while (done < len) {
        uint64_t off = offset + done;
        uint32_t logical = (uint32_t)(off / bs);
        uint32_t boff = (uint32_t)(off % bs);
        uint32_t chunk = bs - boff;
        if (chunk > len - done) chunk = (uint32_t)(len - done);

        bool fresh = false;
        uint32_t phys = map_block(fs, ino, in, logical, false, &fresh);
        if (phys == 0) {
            memset(p + done, 0, chunk); /* hole */
        } else {
            if (!read_block(fs, phys, fs->b_data)) break;
            memcpy(p + done, fs->b_data + boff, chunk);
        }
        done += chunk;
    }
    return done;
}

uint64_t ext4_write(ext4_fs_t *fs, uint64_t ino, uint64_t offset,
                    const void *buf, uint64_t len) {
    if (fs == NULL || buf == NULL || len == 0) return 0;
    if (fs->read_only) return 0;
    ext4_inode_t probe;
    if (!inode_load(fs, ino, &probe)) return 0;
    ext4_inode_t *in = inode_cached(fs);
    if (!EXT4_S_ISREG(in->mode)) return 0;

    uint32_t bs = fs->block_size;
    const uint8_t *p = buf;
    uint64_t done = 0;
    while (done < len) {
        uint64_t off = offset + done;
        uint32_t logical = (uint32_t)(off / bs);
        uint32_t boff = (uint32_t)(off % bs);
        uint32_t chunk = bs - boff;
        if (chunk > len - done) chunk = (uint32_t)(len - done);

        bool fresh = false;
        uint32_t phys = map_block(fs, ino, in, logical, true, &fresh);
        if (phys == 0) break;

        bool whole = (boff == 0 && chunk == bs);
        if (fresh) {
            /* A fresh block must not expose stale disk contents. */
            if (!whole) memset(fs->b_data, 0, bs);
        } else if (!whole) {
            if (!read_block(fs, phys, fs->b_data)) break;
        }
        memcpy(fs->b_data + boff, p + done, chunk);
        if (!write_block(fs, phys, fs->b_data)) break;
        done += chunk;
    }

    uint64_t size = inode_size_of(in);
    if (offset + done > size) inode_set_size(in, offset + done);
    in->mtime = in->ctime = now_secs();
    inode_store(fs, ino);
    return done;
}

bool ext4_lookup(ext4_fs_t *fs, uint64_t dir_ino, const char *name,
                 ext4_stat_t *out) {
    uint8_t nlen;
    if (fs == NULL || !name_valid(name, &nlen)) return false;
    ext4_inode_t dir;
    if (!inode_load(fs, dir_ino, &dir)) return false;
    if (!EXT4_S_ISDIR(dir.mode)) return false;

    lookup_ctx_t c;
    memset(&c, 0, sizeof(c));
    c.name = name;
    c.nlen = nlen;
    dir_walk(fs, dir_ino, &dir, lookup_cb, &c);
    if (c.found && out) *out = c.st;
    return c.found;
}

typedef struct {
    ext4_dirent_cb user_cb;
    void *user_ctx;
    uint64_t count;
    bool stop;
} readdir_ctx_t;

static bool readdir_cb(ext4_fs_t *fs, ext4_dent_t *d, void *vctx) {
    readdir_ctx_t *c = vctx;
    ext4_stat_t st;
    if (!ext4_stat(fs, d->inode, &st)) return true;
    c->count++;
    if (c->user_cb && !c->user_cb(c->user_ctx, &st, d->name, d->name_len)) {
        c->stop = true;
        return false;
    }
    return true;
}

uint64_t ext4_readdir(ext4_fs_t *fs, uint64_t dir_ino, ext4_dirent_cb cb,
                      void *ctx) {
    if (fs == NULL) return 0;
    ext4_inode_t dir;
    if (!inode_load(fs, dir_ino, &dir)) return 0;
    if (!EXT4_S_ISDIR(dir.mode)) return 0;

    readdir_ctx_t rc = { .user_cb = cb, .user_ctx = ctx, .count = 0,
                         .stop = false };
    dir_walk(fs, dir_ino, &dir, readdir_cb, &rc);
    return rc.count;
}

bool ext4_stat(ext4_fs_t *fs, uint64_t ino, ext4_stat_t *out) {
    if (fs == NULL || out == NULL) return false;
    ext4_inode_t in;
    if (!inode_load(fs, ino, &in)) return false;
    out->ino = ino;
    out->mode = in.mode;
    out->size = inode_size_of(&in);
    out->links = in.links;
    out->mtime = in.mtime;
    return true;
}

uint64_t ext4_root(ext4_fs_t *fs) { return fs ? EXT4_ROOT_INO : 0; }

const char *ext4_label(ext4_fs_t *fs) { return fs ? fs->label : ""; }

block_device_t *ext4_device(ext4_fs_t *fs) { return fs ? fs->dev : NULL; }

void ext4_space(ext4_fs_t *fs, uint64_t *total_bytes, uint64_t *free_bytes,
                uint64_t *total_inodes, uint64_t *free_inodes) {
    if (fs == NULL) return;
    if (total_bytes) *total_bytes = fs->blocks_count * fs->block_size;
    if (free_bytes) *free_bytes = sb_free_blocks(fs) * fs->block_size;
    if (total_inodes) *total_inodes = fs->inodes_count;
    if (free_inodes) *free_inodes = SB(fs)->free_inodes_count;
}

/* ------------------------------------------------------------------ */
/* Mount                                                               */
/* ------------------------------------------------------------------ */

static const struct { uint32_t bit; const char *name; } g_bad_incompat[] = {
    { INCOMPAT_COMPRESSION, "compression" },
    { INCOMPAT_RECOVER,     "journal needs recovery" },
    { INCOMPAT_JOURNAL_DEV, "external journal" },
    { INCOMPAT_META_BG,     "meta_bg" },
    { INCOMPAT_MMP,         "mmp" },
    { INCOMPAT_DIRDATA,     "dirdir" },
    { INCOMPAT_INLINE_DATA, "inline_data" },
    { INCOMPAT_ENCRYPT,     "encrypt" },
    { INCOMPAT_CASEFOLD,    "casefold" },
};

ext4_fs_t *ext4_mount(block_device_t *dev, bool read_only) {
    if (dev == NULL || dev->read == NULL || dev->block_size == 0) return NULL;

    ext4_fs_t *fs = kzalloc(sizeof(*fs));
    if (fs == NULL) return NULL;
    fs->dev = dev;
    fs->read_only = read_only;

    fs->sb = kzalloc(EXT4_SUPER_LEN);
    if (fs->sb == NULL || !read_bytes(fs, EXT4_SUPER_OFF, fs->sb, EXT4_SUPER_LEN)) {
        console_write("[ext4] "); console_write(dev->name);
        console_write(": cannot read superblock\n");
        ext4_unmount(fs);
        return NULL;
    }
    ext4_sb_t *sb = fs->sb;

    if (sb->magic != EXT4_MAGIC) {
        console_write("[ext4] "); console_write(dev->name);
        console_write(": no ext4 signature (magic 0x");
        console_write_hex(sb->magic);
        console_write(")\n");
        ext4_unmount(fs);
        return NULL;
    }
    if (sb->rev_level == 0) sb->inode_size = EXT4_INODE_MIN; /* ext2 rev 0 */

    fs->block_size = 1024u << sb->log_block_size;
    if (fs->block_size < 1024u || fs->block_size > 4096u ||
        (fs->block_size % dev->block_size) != 0) {
        console_write("[ext4] unsupported block size\n");
        ext4_unmount(fs);
        return NULL;
    }

    fs->incompat = sb->feature_incompat;
    fs->ro_compat = sb->feature_ro_compat;
    for (size_t i = 0; i < sizeof(g_bad_incompat) / sizeof(g_bad_incompat[0]);
         i++) {
        if (fs->incompat & g_bad_incompat[i].bit) {
            console_write("[ext4] unsupported feature: ");
            console_write(g_bad_incompat[i].name);
            console_putc('\n');
            ext4_unmount(fs);
            return NULL;
        }
    }
    if (fs->ro_compat & RO_BIGALLOC) {
        console_write("[ext4] unsupported feature: bigalloc\n");
        ext4_unmount(fs);
        return NULL;
    }

    fs->has_extents = (fs->incompat & INCOMPAT_EXTENTS) != 0;
    fs->desc_size = (fs->incompat & INCOMPAT_64BIT)
                  ? (sb->desc_size ? sb->desc_size : 32u) : 32u;
    if (fs->desc_size < 32u || fs->desc_size > 256u ||
        (fs->desc_size & (fs->desc_size - 1u))) {
        console_write("[ext4] bad group descriptor size\n");
        ext4_unmount(fs);
        return NULL;
    }

    fs->blocks_per_group = sb->blocks_per_group;
    fs->inodes_per_group = sb->inodes_per_group;
    fs->inode_size = sb->inode_size ? sb->inode_size : EXT4_INODE_MIN;
    fs->first_data_block = sb->first_data_block;
    fs->first_ino = sb->first_ino ? sb->first_ino : 11;
    fs->blocks_count = sb_blocks(fs);
    fs->inodes_count = sb->inodes_count;

    if (fs->blocks_per_group == 0 || fs->inodes_per_group == 0 ||
        fs->blocks_count == 0 || fs->inodes_count == 0 ||
        fs->inode_size < EXT4_INODE_MIN || fs->inode_size > fs->block_size) {
        console_write("[ext4] inconsistent superblock geometry\n");
        ext4_unmount(fs);
        return NULL;
    }

    fs->groups = (uint32_t)((fs->blocks_count - fs->first_data_block +
                             fs->blocks_per_group - 1) / fs->blocks_per_group);
    if (fs->groups == 0 || fs->groups > 8192) {
        console_write("[ext4] implausible group count\n");
        ext4_unmount(fs);
        return NULL;
    }

    fs->gds = kzalloc((size_t)fs->groups * fs->desc_size);
    fs->b_data = kzalloc(fs->block_size);
    fs->b_leaf = kzalloc(fs->block_size);
    fs->b_bitmap = kzalloc(fs->block_size);
    fs->b_inode = kzalloc(fs->inode_size);
    if (fs->gds == NULL || fs->b_data == NULL || fs->b_leaf == NULL ||
        fs->b_bitmap == NULL || fs->b_inode == NULL) {
        console_write("[ext4] out of memory\n");
        ext4_unmount(fs);
        return NULL;
    }

    uint64_t gd_off = (uint64_t)(fs->first_data_block + 1) * fs->block_size;
    if (!read_bytes(fs, gd_off, fs->gds,
                    (uint64_t)fs->groups * fs->desc_size)) {
        console_write("[ext4] cannot read group descriptors\n");
        ext4_unmount(fs);
        return NULL;
    }

    memcpy(fs->label, sb->volume_name, 16);
    fs->label[16] = '\0';
    if (fs->label[0] == '\0') strcpy(fs->label, "(none)");

    console_write("[ext4] mounted '");
    console_write(fs->label);
    console_write("' on ");
    console_write(dev->name);
    console_write(": block ");
    console_write_dec(fs->block_size);
    console_write(", ");
    console_write_dec(fs->groups);
    console_write(" groups, ");
    console_write_dec(fs->inodes_count);
    console_write(" inodes");
    if (fs->read_only) console_write(", read-only");
    console_putc('\n');

    g_ext4_current = fs;
    return fs;
}

void ext4_unmount(ext4_fs_t *fs) {
    if (fs == NULL) return;
    if (g_ext4_current == fs) {
        g_ext4_current = NULL;
    }
    kfree(fs->b_inode);
    kfree(fs->b_bitmap);
    kfree(fs->b_leaf);
    kfree(fs->b_data);
    kfree(fs->gds);
    kfree(fs->sb);
    kfree(fs);
}
