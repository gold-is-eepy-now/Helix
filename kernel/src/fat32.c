/* fat32.c — read-only FAT32 driver.
 *
 * Written from Microsoft's "FAT: General Overview of On-Disk Format". Section names in
 * the comments refer to that document. See fat32.h for the scope of stage 1.
 */

#include "fat32.h"

#include "block_device.h"
#include "heap.h"
#include "kstring.h"

#define FAT_MASK       0x0FFFFFFFu   /* a FAT32 entry is 28 bits; the top 4 are reserved */
#define FAT_EOC_MIN    0x0FFFFFF8u   /* values from here up mark the end of a chain */
#define FAT_BAD        0x0FFFFFF7u   /* a cluster marked bad */
#define FAT32_MIN_CLUSTERS 65525u    /* fewer data clusters than this means FAT12 or FAT16 */
#define FAT32_MAX_CLUSTERS 0x0FFFFFF5u

#define DIR_ENTRY_SIZE     32u
#define LFN_MAX_ENTRIES    20u       /* 20 entries of 13 units cover the 255-unit maximum */
#define LFN_UNITS_PER_ENTRY 13u
#define LFN_UNITS          (LFN_MAX_ENTRIES * LFN_UNITS_PER_ENTRY)
#define ATTR_LONG_NAME_MASK 0x3Fu
#define ATTR_LONG_NAME      0x0Fu    /* read-only | hidden | system | volume id */

struct fat32_fs {
    block_device_t *dev;
    uint64_t first_lba;              /* device block where the volume's sector 0 sits */
    uint32_t bytes_per_sector;
    uint32_t ratio;                  /* device blocks per FAT sector */
    uint32_t sectors_per_cluster;
    uint32_t cluster_bytes;
    uint32_t reserved_sectors;
    uint32_t fat_sectors;            /* sectors in one FAT */
    uint32_t fat_start;              /* first sector of the active FAT */
    uint32_t first_data_sector;
    uint32_t cluster_count;          /* data clusters; valid numbers are 2 .. cluster_count + 1 */
    uint32_t root_cluster;
    char label[12];
    uint8_t *cluster_buf;            /* scratch: one cluster, shared by scans and reads */
    uint8_t *fat_buf;                /* one cached FAT sector */
    uint32_t fat_buf_sector;         /* the FAT sector held in fat_buf, or 0xFFFFFFFF */
};

static const char *g_mount_error = "";

const char *fat32_mount_error(void) { return g_mount_error; }

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ------------------------------------------------------------------ sector access */

/* Read count FAT sectors, numbered from the start of the volume. */
static bool read_sectors(fat32_fs_t *fs, uint32_t sector, uint32_t count, void *buf) {
    uint64_t lba = fs->first_lba + (uint64_t)sector * fs->ratio;
    uint64_t blocks = (uint64_t)count * fs->ratio;
    return block_device_read(fs->dev, lba, buf, blocks) == blocks;
}

static bool cluster_valid(const fat32_fs_t *fs, uint32_t c) {
    return c >= 2 && c <= fs->cluster_count + 1u;
}

static bool read_cluster(fat32_fs_t *fs, uint32_t cluster, void *buf) {
    if (!cluster_valid(fs, cluster)) return false;
    uint32_t sector = fs->first_data_sector + (cluster - 2u) * fs->sectors_per_cluster;
    return read_sectors(fs, sector, fs->sectors_per_cluster, buf);
}

/* Look up the FAT entry for a cluster (section "FAT Data Structure").
 * Returns 1 and sets *next if the chain continues, 0 at the end of the chain,
 * and -1 if the FAT cannot be read or the entry is not a valid cluster number. */
static int fat_next(fat32_fs_t *fs, uint32_t cluster, uint32_t *next) {
    if (!cluster_valid(fs, cluster)) return -1;
    uint64_t offset = (uint64_t)cluster * 4u;
    uint32_t sector = fs->fat_start + (uint32_t)(offset / fs->bytes_per_sector);
    uint32_t in_sector = (uint32_t)(offset % fs->bytes_per_sector);
    if (fs->fat_buf_sector != sector) {
        if (!read_sectors(fs, sector, 1, fs->fat_buf)) return -1;
        fs->fat_buf_sector = sector;
    }
    uint32_t v = le32(fs->fat_buf + in_sector) & FAT_MASK;
    if (v >= FAT_EOC_MIN) return 0;
    if (v == FAT_BAD || !cluster_valid(fs, v)) return -1;
    *next = v;
    return 1;
}

/* ------------------------------------------------------------------ names */

/* The checksum a short entry stores in its long-name entries (section "Long Directory
 * Entries"): rotate right by one, then add the next byte. */
static uint8_t short_checksum(const uint8_t *name11) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) {
        sum = (uint8_t)(((sum & 1u) ? 0x80u : 0u) + (sum >> 1) + name11[i]);
    }
    return sum;
}

static char ascii_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static bool names_equal_ci(const char *a, const char *b) {
    while (*a != '\0' && *b != '\0') {
        if (ascii_lower(*a) != ascii_lower(*b)) return false;
        a++;
        b++;
    }
    return *a == *b;
}

/* "NAME    EXT" to "name.ext". Byte 0 of 0x05 stands for 0xE5, and the NT flags in
 * byte 12 say whether the base (0x08) and the extension (0x10) show in lower case. */
static void short_name(const uint8_t *e, char *out) {
    char base[8];
    char ext[3];
    for (int i = 0; i < 8; i++) base[i] = (char)e[i];
    for (int i = 0; i < 3; i++) ext[i] = (char)e[8 + i];
    if ((uint8_t)base[0] == 0x05) base[0] = (char)0xE5;
    int base_len = 8;
    while (base_len > 0 && base[base_len - 1] == ' ') base_len--;
    int ext_len = 3;
    while (ext_len > 0 && ext[ext_len - 1] == ' ') ext_len--;
    bool lower_base = (e[12] & 0x08u) != 0;
    bool lower_ext = (e[12] & 0x10u) != 0;
    size_t n = 0;
    for (int i = 0; i < base_len; i++) out[n++] = lower_base ? ascii_lower(base[i]) : base[i];
    if (ext_len > 0) {
        out[n++] = '.';
        for (int i = 0; i < ext_len; i++) out[n++] = lower_ext ? ascii_lower(ext[i]) : ext[i];
    }
    out[n] = '\0';
}

/* A volume label uses all 11 bytes as one name, with trailing spaces removed. */
static void label_name(const uint8_t *e, char *out) {
    int n = 11;
    while (n > 0 && e[n - 1] == ' ') n--;
    for (int i = 0; i < n; i++) out[i] = (char)e[i];
    out[n] = '\0';
}

/* Long names are UTF-16; convert to UTF-8, stopping at the terminator or when full. */
static void utf16_to_utf8(const uint16_t *u, char *out, size_t cap) {
    size_t n = 0;
    uint32_t units = 0;
    while (units < LFN_UNITS && u[units] != 0x0000 && u[units] != 0xFFFF) units++;
    for (uint32_t i = 0; i < units; i++) {
        uint32_t cp = u[i];
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < units && u[i + 1] >= 0xDC00 && u[i + 1] < 0xE000) {
            cp = 0x10000u + ((cp - 0xD800u) << 10) + (uint32_t)(u[i + 1] - 0xDC00u);
            i++;
        } else if (cp >= 0xD800 && cp < 0xE000) {
            cp = '?';                                 /* a lone surrogate is not valid text */
        }
        if (n + 5 > cap) break;
        if (cp < 0x80) {
            out[n++] = (char)cp;
        } else if (cp < 0x800) {
            out[n++] = (char)(0xC0 | (cp >> 6));
            out[n++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out[n++] = (char)(0xE0 | (cp >> 12));
            out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[n++] = (char)(0x80 | (cp & 0x3F));
        } else {
            out[n++] = (char)(0xF0 | (cp >> 18));
            out[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[n++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    out[n] = '\0';
}

/* ------------------------------------------------------------------ directories */

typedef bool (*scan_cb)(void *ctx, const fat32_entry_t *entry);

/* Walk every entry of the directory that starts at dir_cluster, following its chain.
 * The callback sees each live entry, including the volume label, with its long name when
 * one is present and its checksum matches. It returns true to stop. Returns false only on
 * an I/O or chain error. */
static bool dir_scan(fat32_fs_t *fs, uint32_t dir_cluster, scan_cb cb, void *ctx) {
    uint16_t lfn[LFN_UNITS];
    bool lfn_active = false;
    uint8_t lfn_expect = 0;
    uint8_t lfn_sum = 0;
    uint32_t cluster = dir_cluster;
    uint32_t guard = 0;

    for (;;) {
        if (!read_cluster(fs, cluster, fs->cluster_buf)) return false;
        for (uint32_t off = 0; off < fs->cluster_bytes; off += DIR_ENTRY_SIZE) {
            const uint8_t *e = fs->cluster_buf + off;
            if (e[0] == 0x00) return true;                    /* no entries after this one */
            if (e[0] == 0xE5) { lfn_active = false; continue; } /* a deleted entry */

            uint8_t attr = e[11];
            if ((attr & ATTR_LONG_NAME_MASK) == ATTR_LONG_NAME) {
                uint8_t seq = e[0] & 0x3Fu;
                if (e[0] & 0x40u) {                           /* the last piece comes first */
                    if (seq == 0 || seq > LFN_MAX_ENTRIES) { lfn_active = false; continue; }
                    memset(lfn, 0xFF, sizeof lfn);
                    lfn_active = true;
                    lfn_expect = seq;
                    lfn_sum = e[13];
                } else if (!lfn_active || seq != lfn_expect || e[13] != lfn_sum) {
                    lfn_active = false;
                    continue;
                }
                uint16_t *dst = &lfn[(uint32_t)(seq - 1u) * LFN_UNITS_PER_ENTRY];
                for (int i = 0; i < 5; i++) dst[i] = le16(e + 1 + 2 * i);
                for (int i = 0; i < 6; i++) dst[5 + i] = le16(e + 14 + 2 * i);
                for (int i = 0; i < 2; i++) dst[11 + i] = le16(e + 28 + 2 * i);
                lfn_expect = (uint8_t)(seq - 1u);
                continue;
            }

            fat32_entry_t ent;
            memset(&ent, 0, sizeof ent);
            ent.attr = attr;
            if (attr & FAT32_ATTR_VOLUME_ID) {
                label_name(e, ent.name);
            } else if (lfn_active && lfn_expect == 0 && short_checksum(e) == lfn_sum) {
                utf16_to_utf8(lfn, ent.name, sizeof ent.name);
            } else {
                short_name(e, ent.name);
            }
            lfn_active = false;
            ent.is_dir = (attr & FAT32_ATTR_DIRECTORY) != 0;
            ent.first_cluster = ((uint32_t)le16(e + 20) << 16) | le16(e + 26);
            ent.size = ent.is_dir ? 0u : le32(e + 28);
            if (cb(ctx, &ent)) return true;
        }

        uint32_t next = 0;
        int r = fat_next(fs, cluster, &next);
        if (r == 0) return true;                              /* end of the directory chain */
        if (r < 0 || ++guard > fs->cluster_count) return false;
        cluster = next;
    }
}

typedef struct {
    const char *name;
    bool found;
    fat32_entry_t entry;
} find_ctx_t;

static bool find_cb(void *vctx, const fat32_entry_t *e) {
    find_ctx_t *ctx = vctx;
    if (e->attr & FAT32_ATTR_VOLUME_ID) return false;
    if (!names_equal_ci(e->name, ctx->name)) return false;
    ctx->found = true;
    ctx->entry = *e;
    return true;
}

/* Walk a path from the root. Components "." and ".." work; ".." at the root stays there. */
static bool resolve(fat32_fs_t *fs, const char *path, fat32_entry_t *out) {
    fat32_entry_t cur;
    memset(&cur, 0, sizeof cur);
    cur.name[0] = '/';
    cur.is_dir = true;
    cur.attr = FAT32_ATTR_DIRECTORY;
    cur.first_cluster = fs->root_cluster;

    const char *p = path;
    while (*p != '\0') {
        while (*p == '/') p++;
        if (*p == '\0') break;
        char comp[FAT32_NAME_BYTES];
        size_t n = 0;
        while (*p != '\0' && *p != '/') {
            if (n + 1 >= sizeof comp) return false;
            comp[n++] = *p++;
        }
        comp[n] = '\0';
        if (strcmp(comp, ".") == 0) continue;
        if (!cur.is_dir) return false;

        find_ctx_t f;
        f.name = comp;
        f.found = false;
        if (!dir_scan(fs, cur.first_cluster, find_cb, &f)) return false;
        if (!f.found) {
            if (strcmp(comp, "..") == 0 && cur.first_cluster == fs->root_cluster) continue;
            return false;
        }
        cur = f.entry;
        /* A ".." entry that points at the root stores cluster 0. */
        if (strcmp(comp, "..") == 0 && cur.first_cluster == 0) cur.first_cluster = fs->root_cluster;
    }
    *out = cur;
    return true;
}

/* ------------------------------------------------------------------ mount */

static void fs_free(fat32_fs_t *fs) {
    if (fs == NULL) return;
    if (fs->cluster_buf != NULL) kfree(fs->cluster_buf);
    if (fs->fat_buf != NULL) kfree(fs->fat_buf);
    kfree(fs);
}

typedef struct {
    char label[12];
    bool found;
} label_ctx_t;

static bool label_cb(void *vctx, const fat32_entry_t *e) {
    label_ctx_t *ctx = vctx;
    if (!(e->attr & FAT32_ATTR_VOLUME_ID)) return false;
    size_t n = strlen(e->name);
    if (n > 11) n = 11;
    memcpy(ctx->label, e->name, n);
    ctx->label[n] = '\0';
    ctx->found = true;
    return true;
}

fat32_fs_t *fat32_mount(block_device_t *dev, uint64_t first_lba) {
    g_mount_error = "";
    if (dev == NULL || dev->read == NULL || dev->block_size < 512 || dev->block_size > 4096) {
        g_mount_error = "not a usable block device";
        return NULL;
    }
    uint8_t *bs = kmalloc(dev->block_size);
    if (bs == NULL) {
        g_mount_error = "out of memory";
        return NULL;
    }
    if (block_device_read(dev, first_lba, bs, 1) != 1) {
        kfree(bs);
        g_mount_error = "cannot read the boot sector";
        return NULL;
    }

    /* Boot sector fields (section "Boot Sector and BPB"). */
    uint32_t bps = le16(bs + 11);
    uint32_t spc = bs[13];
    uint32_t reserved = le16(bs + 14);
    uint32_t num_fats = bs[16];
    uint32_t root_ent = le16(bs + 17);
    uint32_t tot16 = le16(bs + 19);
    uint32_t fatsz16 = le16(bs + 22);
    uint32_t tot32 = le32(bs + 32);
    uint32_t fatsz32 = le32(bs + 36);
    uint32_t ext_flags = le16(bs + 40);
    uint32_t fs_ver = le16(bs + 42);
    uint32_t root_clus = le32(bs + 44);
    bool sig_ok = bs[510] == 0x55 && bs[511] == 0xAA;
    bool has_label = bs[66] == 0x29;
    char boot_label[12];
    memset(boot_label, 0, sizeof boot_label);
    if (has_label) {
        label_name(bs + 71, boot_label);       /* BS_VolLab: 11 bytes at offset 71 */
    }
    kfree(bs);

    if (!sig_ok) { g_mount_error = "no boot sector signature (55 AA)"; return NULL; }
    if (bps != 512 && bps != 1024 && bps != 2048 && bps != 4096) {
        g_mount_error = "bytes per sector is not 512, 1024, 2048 or 4096";
        return NULL;
    }
    if (bps < dev->block_size || bps % dev->block_size != 0) {
        g_mount_error = "sector size does not fit the device block size";
        return NULL;
    }
    if (spc == 0 || spc > 128 || (spc & (spc - 1)) != 0) {
        g_mount_error = "sectors per cluster is not a power of two up to 128";
        return NULL;
    }
    if (reserved == 0) { g_mount_error = "no reserved sectors"; return NULL; }
    if (num_fats == 0 || num_fats > 4) { g_mount_error = "bad number of FATs"; return NULL; }
    if (root_ent != 0 || tot16 != 0 || fatsz16 != 0 || fatsz32 == 0 || tot32 == 0) {
        g_mount_error = "boot sector is FAT12 or FAT16 style";
        return NULL;
    }
    if (fs_ver != 0) { g_mount_error = "unsupported FAT32 version"; return NULL; }
    if (root_clus < 2) { g_mount_error = "bad root cluster"; return NULL; }

    /* The type is decided only by the number of data clusters (section "FAT Type
     * Determination"). */
    uint64_t meta = (uint64_t)reserved + (uint64_t)num_fats * fatsz32;
    if ((uint64_t)tot32 <= meta) { g_mount_error = "no data area"; return NULL; }
    uint64_t clusters = ((uint64_t)tot32 - meta) / spc;
    if (clusters < FAT32_MIN_CLUSTERS) {
        g_mount_error = "too few clusters for FAT32 (FAT12 and FAT16 are not supported)";
        return NULL;
    }
    if (clusters > FAT32_MAX_CLUSTERS) { g_mount_error = "too many clusters"; return NULL; }
    if ((uint64_t)fatsz32 * bps / 4u < clusters + 2u) {
        g_mount_error = "the FAT is too small for the cluster count";
        return NULL;
    }
    uint32_t ratio = bps / (uint32_t)dev->block_size;
    if (dev->block_count != 0 && first_lba + (uint64_t)tot32 * ratio > dev->block_count) {
        g_mount_error = "the volume is larger than the device";
        return NULL;
    }
    uint32_t active_fat = 0;
    if (ext_flags & 0x80u) {                       /* mirroring is off: one FAT is active */
        active_fat = ext_flags & 0x0Fu;
        if (active_fat >= num_fats) { g_mount_error = "bad active FAT"; return NULL; }
    }
    if ((uint64_t)root_clus > clusters + 1u) { g_mount_error = "root cluster out of range"; return NULL; }

    fat32_fs_t *fs = kzalloc(sizeof *fs);
    if (fs == NULL) { g_mount_error = "out of memory"; return NULL; }
    fs->dev = dev;
    fs->first_lba = first_lba;
    fs->bytes_per_sector = bps;
    fs->ratio = ratio;
    fs->sectors_per_cluster = spc;
    fs->cluster_bytes = bps * spc;
    fs->reserved_sectors = reserved;
    fs->fat_sectors = fatsz32;
    fs->fat_start = reserved + active_fat * fatsz32;
    fs->first_data_sector = (uint32_t)meta;
    fs->cluster_count = (uint32_t)clusters;
    fs->root_cluster = root_clus;
    fs->fat_buf_sector = 0xFFFFFFFFu;
    memcpy(fs->label, boot_label, sizeof fs->label);
    fs->label[11] = '\0';
    fs->cluster_buf = kmalloc(fs->cluster_bytes);
    fs->fat_buf = kmalloc(bps);
    if (fs->cluster_buf == NULL || fs->fat_buf == NULL) {
        fs_free(fs);
        g_mount_error = "out of memory";
        return NULL;
    }

    /* The real label lives in the root directory; the boot sector copy is often stale. */
    label_ctx_t lc;
    memset(&lc, 0, sizeof lc);
    if (!dir_scan(fs, fs->root_cluster, label_cb, &lc)) {
        fs_free(fs);
        g_mount_error = "cannot read the root directory";
        return NULL;
    }
    if (lc.found) memcpy(fs->label, lc.label, sizeof fs->label);
    return fs;
}

void fat32_unmount(fat32_fs_t *fs) { fs_free(fs); }

const char *fat32_label(fat32_fs_t *fs) { return fs != NULL ? fs->label : ""; }

void fat32_info(fat32_fs_t *fs, uint32_t *bytes_per_sector, uint32_t *sectors_per_cluster,
                uint32_t *clusters, uint32_t *free_clusters) {
    if (fs == NULL) return;
    if (bytes_per_sector) *bytes_per_sector = fs->bytes_per_sector;
    if (sectors_per_cluster) *sectors_per_cluster = fs->sectors_per_cluster;
    if (clusters) *clusters = fs->cluster_count;
    if (free_clusters == NULL) return;

    /* Count the free entries (value 0) by reading the active FAT sector by sector. */
    uint32_t per_sector = fs->bytes_per_sector / 4u;
    uint32_t entries = fs->cluster_count + 2u;
    uint32_t free_count = 0;
    for (uint32_t s = 0; (uint64_t)s * per_sector < entries; s++) {
        if (!read_sectors(fs, fs->fat_start + s, 1, fs->fat_buf)) break;
        fs->fat_buf_sector = fs->fat_start + s;
        for (uint32_t i = 0; i < per_sector; i++) {
            uint64_t c = (uint64_t)s * per_sector + i;
            if (c < 2) continue;
            if (c >= entries) break;
            if ((le32(fs->fat_buf + i * 4u) & FAT_MASK) == 0) free_count++;
        }
    }
    *free_clusters = free_count;
}

/* ------------------------------------------------------------------ public lookups */

bool fat32_stat(fat32_fs_t *fs, const char *path, fat32_entry_t *out) {
    if (fs == NULL || path == NULL || out == NULL) return false;
    return resolve(fs, path, out);
}

typedef struct {
    fat32_dirent_cb cb;
    void *ctx;
} list_ctx_t;

static bool list_cb(void *vctx, const fat32_entry_t *e) {
    list_ctx_t *l = vctx;
    if (e->attr & FAT32_ATTR_VOLUME_ID) return false;      /* the label is not a file */
    return l->cb(l->ctx, e);
}

bool fat32_list(fat32_fs_t *fs, const char *path, fat32_dirent_cb cb, void *ctx) {
    if (fs == NULL || path == NULL || cb == NULL) return false;
    fat32_entry_t dir;
    if (!resolve(fs, path, &dir) || !dir.is_dir) return false;
    list_ctx_t l = { cb, ctx };
    return dir_scan(fs, dir.first_cluster, list_cb, &l);
}

int64_t fat32_read(fat32_fs_t *fs, const char *path, uint64_t offset, void *buf, uint64_t len) {
    if (fs == NULL || path == NULL || buf == NULL) return -1;
    fat32_entry_t e;
    if (!resolve(fs, path, &e) || e.is_dir) return -1;
    if (offset >= e.size) return 0;
    uint64_t remaining = (uint64_t)e.size - offset;
    if (len > remaining) len = remaining;
    if (len == 0) return 0;
    if (!cluster_valid(fs, e.first_cluster)) return -1;     /* bytes to read but no clusters */

    uint32_t cluster = e.first_cluster;
    uint64_t skip = offset / fs->cluster_bytes;
    uint32_t in_cluster = (uint32_t)(offset % fs->cluster_bytes);
    for (uint64_t i = 0; i < skip; i++) {
        uint32_t next = 0;
        if (fat_next(fs, cluster, &next) != 1) return -1;   /* the chain is shorter than the file */
        cluster = next;
    }

    uint64_t done = 0;
    while (done < len) {
        if (!read_cluster(fs, cluster, fs->cluster_buf)) return -1;
        uint64_t chunk = fs->cluster_bytes - in_cluster;
        if (chunk > len - done) chunk = len - done;
        memcpy((uint8_t *)buf + done, fs->cluster_buf + in_cluster, (size_t)chunk);
        done += chunk;
        in_cluster = 0;
        if (done < len) {
            uint32_t next = 0;
            if (fat_next(fs, cluster, &next) != 1) return -1;
            cluster = next;
        }
    }
    return (int64_t)done;
}
