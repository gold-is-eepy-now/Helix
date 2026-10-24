/* fat32.c — conservative new-file writes to a FAT32 USB volume. */
#include "fat32.h"

#include <stdint.h>

#include "driver.h"
#include "kstring.h"
#include "object.h"

#define FAT_SECTOR_SIZE 512u
#define FAT32_MAX_FILE_SIZE 8192u
#define FAT32_MAX_FILE_CLUSTERS 16u

typedef struct {
    handle_t read_cap;
    handle_t write_cap;
    uint64_t partition_lba;
    uint64_t device_sectors;
    uint64_t fat_lba;
    uint64_t data_lba;
    uint64_t fat_sectors;
    uint64_t cluster_count;
    uint32_t sectors_per_cluster;
    uint32_t fat_count;
    uint32_t root_cluster;
} fat32_volume_t;

static uint16_t get16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static bool read_sector(fat32_volume_t *v, uint64_t lba, uint8_t sector[512]) {
    return lba < v->device_sectors && driver_read(object_kernel_handles(), v->read_cap, lba,
                       sector, FAT_SECTOR_SIZE);
}

static bool write_sector(fat32_volume_t *v, uint64_t lba,
                         const uint8_t sector[512]) {
    return lba < v->device_sectors && driver_write(object_kernel_handles(), v->write_cap, lba,
                        sector, FAT_SECTOR_SIZE);
}

static bool parse_boot(fat32_volume_t *v, uint64_t base, const uint8_t *b) {
    uint32_t bytes_per_sector = get16(b + 11);
    uint32_t spc = b[13];
    uint32_t reserved = get16(b + 14);
    uint32_t fats = b[16];
    uint32_t total = get16(b + 19);
    if (total == 0) total = get32(b + 32);
    uint32_t fat16 = get16(b + 22);
    uint32_t fat_size = get32(b + 36);
    uint32_t root = get32(b + 44) & 0x0fffffff;
    if (b[510] != 0x55 || b[511] != 0xaa || bytes_per_sector != 512 ||
        spc == 0 || spc > 128 || (spc & (spc - 1)) != 0 || reserved == 0 ||
        fats == 0 || fats > 2 || fat16 != 0 || fat_size == 0 || total == 0 ||
        root < 2) return false;
    uint64_t overhead = (uint64_t)reserved + (uint64_t)fats * fat_size;
    if (overhead >= total || base > v->device_sectors ||
        total > v->device_sectors - base) return false;
    uint64_t clusters = (total - overhead) / spc;
    if (clusters == 0 || clusters > 0x0fffffee || root > clusters + 1)
        return false;
    v->partition_lba = base;
    v->fat_lba = base + reserved;
    v->data_lba = base + overhead;
    v->fat_sectors = fat_size;
    v->cluster_count = clusters;
    v->sectors_per_cluster = spc;
    v->fat_count = fats;
    v->root_cluster = root;
    return true;
}

static bool mount_any(fat32_volume_t *v) {
    driver_info_t info;
    for (size_t i = 0; i < driver_count(); i++) {
        if (!driver_get_info(i, &info) ||
            strcmp(info.device_class, "block-storage") != 0 ||
            info.read_handle == 0 || info.write_handle == 0 ||
            info.sector_size != FAT_SECTOR_SIZE || info.sector_count == 0)
            continue;
        fat32_volume_t candidate = {0};
        candidate.read_cap = info.read_handle;
        candidate.write_cap = info.write_handle;
        candidate.device_sectors = info.sector_count;
        uint8_t sector[512];
        if (!read_sector(&candidate, 0, sector)) continue;
        if (parse_boot(&candidate, 0, sector)) {
            *v = candidate;
            return true;
        }
        for (uint32_t p = 0; p < 4; p++) {
            const uint8_t *entry = sector + 446 + p * 16;
            if (entry[4] != 0x0b && entry[4] != 0x0c &&
                entry[4] != 0x1b && entry[4] != 0x1c) continue;
            uint32_t start = get32(entry + 8);
            if (start != 0 && read_sector(&candidate, start, sector) &&
                parse_boot(&candidate, start, sector)) {
                *v = candidate;
                return true;
            }
        }
    }
    return false;
}

static bool format_name(const char *name, uint8_t out[11]) {
    size_t stem = 0, ext = 0;
    while (name[stem] != '\0' && name[stem] != '.') {
        if (stem == 8) return false;
        char c = name[stem];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
            return false;
        out[stem] = (uint8_t)c;
        stem++;
    }
    if (stem == 0 || name[stem] != '.') return false;
    size_t i = stem + 1;
    while (name[i] != '\0') {
        if (ext == 3) return false;
        char c = name[i++];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
            return false;
        out[8 + ext++] = (uint8_t)c;
    }
    if (ext != 3 || out[8] != 'T' || out[9] != 'X' || out[10] != 'T')
        return false;
    for (size_t n = stem; n < 8; n++) out[n] = ' ';
    return true;
}

static bool fat_entry(fat32_volume_t *v, uint32_t cluster, uint32_t *value) {
    uint64_t byte = (uint64_t)cluster * 4;
    uint8_t sector[512];
    if (byte / 512 >= v->fat_sectors ||
        !read_sector(v, v->fat_lba + byte / 512, sector)) return false;
    *value = get32(sector + (byte % 512)) & 0x0fffffff;
    return true;
}

static bool find_root_slot(fat32_volume_t *v, const uint8_t wanted[11],
                           uint64_t *slot_lba, uint32_t *slot_offset,
                           bool *existing) {
    uint32_t cluster = v->root_cluster;
    uint32_t seen[32];
    uint8_t sector[512];
    bool have_free_slot = false;
    *existing = false;
    for (uint32_t chain = 0; chain < 32; chain++) {
        if (cluster < 2 || cluster > v->cluster_count + 1) return false;
        for (uint32_t i = 0; i < chain; i++) if (seen[i] == cluster) return false;
        seen[chain] = cluster;
        uint64_t first = v->data_lba + (uint64_t)(cluster - 2) * v->sectors_per_cluster;
        for (uint32_t s = 0; s < v->sectors_per_cluster; s++) {
            if (!read_sector(v, first + s, sector)) return false;
            for (uint32_t off = 0; off < 512; off += 32) {
                uint8_t *entry = sector + off;
                if (entry[0] == 0) {
                    if (!have_free_slot) {
                        *slot_lba = first + s;
                        *slot_offset = off;
                    }
                    return true;
                }
                if (entry[0] == 0xe5) {
                    if (!have_free_slot) {
                        *slot_lba = first + s;
                        *slot_offset = off;
                        have_free_slot = true;
                    }
                    continue;
                }
                if (entry[11] != 0x0f && (entry[11] & 0x08) == 0 &&
                    memcmp(entry, wanted, 11) == 0) {
                    *slot_lba = first + s;
                    *slot_offset = off;
                    *existing = true;
                    return true;
                }
            }
        }
        uint32_t next;
        if (!fat_entry(v, cluster, &next)) return false;
        if (next >= 0x0ffffff8) return have_free_slot;
        cluster = next;
    }
    return false;
}

static bool find_free_clusters(fat32_volume_t *v, uint32_t count,
                               uint32_t out[FAT32_MAX_FILE_CLUSTERS]) {
    uint32_t found = 0;
    uint8_t sector[512];
    for (uint64_t s = 0; s < v->fat_sectors && found < count; s++) {
        if (!read_sector(v, v->fat_lba + s, sector)) return false;
        for (uint32_t off = 0; off < FAT_SECTOR_SIZE && found < count; off += 4) {
            uint64_t cluster = s * (FAT_SECTOR_SIZE / 4) + off / 4;
            if (cluster < 2 || cluster > v->cluster_count + 1) continue;
            if ((get32(sector + off) & 0x0fffffff) == 0)
                out[found++] = (uint32_t)cluster;
        }
    }
    return found == count;
}

static bool write_fat_chain(fat32_volume_t *v, const uint32_t *clusters,
                            uint32_t count) {
    uint8_t sector[512];
    for (uint32_t copy = 0; copy < v->fat_count; copy++) {
        for (uint32_t first = 0; first < count;) {
            uint64_t first_fat_sector = ((uint64_t)clusters[first] * 4) / 512;
            uint64_t lba = v->fat_lba + (uint64_t)copy * v->fat_sectors + first_fat_sector;
            if (!read_sector(v, lba, sector)) return false;
            uint32_t end = first;
            while (end < count &&
                   ((uint64_t)clusters[end] * 4) / 512 == first_fat_sector) {
                uint32_t offset = (clusters[end] * 4u) % 512u;
                uint32_t next = end + 1 < count ? clusters[end + 1] : 0x0fffffff;
                put32(sector + offset,
                    (get32(sector + offset) & 0xf0000000) | next);
                end++;
            }
            if (!write_sector(v, lba, sector)) return false;
            first = end;
        }
    }
    return true;
}

static bool load_file_chain(fat32_volume_t *v, uint32_t first,
                            uint32_t clusters[FAT32_MAX_FILE_CLUSTERS],
                            uint32_t *count_out) {
    uint32_t count = 0;
    while (first >= 2 && first <= v->cluster_count + 1) {
        if (count == FAT32_MAX_FILE_CLUSTERS) return false;
        for (uint32_t i = 0; i < count; i++) if (clusters[i] == first) return false;
        clusters[count++] = first;
        uint32_t next;
        if (!fat_entry(v, first, &next)) return false;
        if (next >= 0x0ffffff8) {
            *count_out = count;
            return true;
        }
        if (next < 2 || next > v->cluster_count + 1) return false;
        first = next;
    }
    if (first == 0) { *count_out = 0; return true; }
    return false;
}

static bool clear_fat_chain(fat32_volume_t *v, const uint32_t *clusters,
                            uint32_t count) {
    uint8_t sector[512];
    for (uint32_t copy = 0; copy < v->fat_count; copy++) {
        for (uint32_t i = 0; i < count; i++) {
            uint64_t byte = (uint64_t)clusters[i] * 4;
            uint64_t lba = v->fat_lba + (uint64_t)copy * v->fat_sectors + byte / 512;
            if (!read_sector(v, lba, sector)) return false;
            uint32_t off = (uint32_t)(byte % 512);
            put32(sector + off, get32(sector + off) & 0xf0000000u);
            if (!write_sector(v, lba, sector)) return false;
        }
    }
    return true;
}

bool fat32_save_usb0_file(const char *name, const char *data, size_t length) {
    if (name == NULL || (length != 0 && data == NULL) ||
        length > FAT32_MAX_FILE_SIZE) return false;
    uint8_t short_name[11];
    if (!format_name(name, short_name)) return false;
    fat32_volume_t volume = {0};
    if (!mount_any(&volume)) return false;

    uint64_t dir_lba;
    uint32_t dir_offset;
    bool existing;
    if (!find_root_slot(&volume, short_name, &dir_lba, &dir_offset,
                        &existing)) return false;

    uint8_t sector[512];
    uint32_t old_clusters[FAT32_MAX_FILE_CLUSTERS];
    uint32_t old_count = 0;
    if (!read_sector(&volume, dir_lba, sector)) return false;
    uint8_t *entry = sector + dir_offset;
    if (existing) {
        if ((entry[11] & 0x11u) != 0) return false; /* read-only or directory */
        uint32_t first = ((uint32_t)entry[20] << 16) |
                         ((uint32_t)entry[21] << 24) |
                         entry[26] | ((uint32_t)entry[27] << 8);
        if (!load_file_chain(&volume, first, old_clusters, &old_count))
            return false;
    }

    uint32_t cluster_bytes = volume.sectors_per_cluster * FAT_SECTOR_SIZE;
    uint32_t count = length == 0 ? 0 : (uint32_t)((length + cluster_bytes - 1) / cluster_bytes);
    uint32_t clusters[FAT32_MAX_FILE_CLUSTERS];
    if (count > FAT32_MAX_FILE_CLUSTERS ||
        (count != 0 && !find_free_clusters(&volume, count, clusters))) return false;

    size_t remaining = length;
    size_t source = 0;
    for (uint32_t c = 0; c < count; c++) {
        uint64_t first = volume.data_lba +
            (uint64_t)(clusters[c] - 2) * volume.sectors_per_cluster;
        size_t cluster_amount = remaining < cluster_bytes ? remaining : cluster_bytes;
        uint32_t sectors_to_write = (uint32_t)((cluster_amount + 511) / 512);
        for (uint32_t s = 0; s < sectors_to_write; s++) {
            memset(sector, 0, sizeof(sector));
            size_t amount = remaining < sizeof(sector) ? remaining : sizeof(sector);
            if (amount != 0) memcpy(sector, data + source, amount);
            if (!write_sector(&volume, first + s, sector)) return false;
            source += amount;
            remaining -= amount;
        }
    }
    if (count != 0 && !write_fat_chain(&volume, clusters, count)) return false;

    if (!read_sector(&volume, dir_lba, sector)) return false;
    entry = sector + dir_offset;
    if (existing) {
        uint32_t first = count != 0 ? clusters[0] : 0;
        entry[20] = (uint8_t)(first >> 16);
        entry[21] = (uint8_t)(first >> 24);
        entry[26] = (uint8_t)first;
        entry[27] = (uint8_t)(first >> 8);
    } else {
        if (entry[0] != 0 && entry[0] != 0xe5) return false;
        memset(entry, 0, 32);
        memcpy(entry, short_name, 11);
        entry[11] = 0x20;
        if (count != 0) {
            entry[20] = (uint8_t)(clusters[0] >> 16);
            entry[21] = (uint8_t)(clusters[0] >> 24);
            entry[26] = (uint8_t)clusters[0];
            entry[27] = (uint8_t)(clusters[0] >> 8);
        }
    }
    put32(entry + 28, (uint32_t)length);
    if (!write_sector(&volume, dir_lba, sector) ||
        !clear_fat_chain(&volume, old_clusters, old_count)) return false;
    return driver_flush(object_kernel_handles(), volume.write_cap);
}

bool fat32_delete_usb0_file(const char *name) {
    if (name == NULL) return false;
    uint8_t short_name[11];
    if (!format_name(name, short_name)) return false;
    fat32_volume_t volume = {0};
    if (!mount_any(&volume)) return false;

    uint64_t dir_lba;
    uint32_t dir_offset;
    bool existing;
    if (!find_root_slot(&volume, short_name, &dir_lba, &dir_offset,
                        &existing) || !existing) return false;
    uint8_t sector[512];
    if (!read_sector(&volume, dir_lba, sector)) return false;
    uint8_t *entry = sector + dir_offset;
    if ((entry[11] & 0x11u) != 0) return false;
    uint32_t first = ((uint32_t)entry[20] << 16) |
                     ((uint32_t)entry[21] << 24) |
                     entry[26] | ((uint32_t)entry[27] << 8);
    uint32_t clusters[FAT32_MAX_FILE_CLUSTERS];
    uint32_t count = 0;
    if (!load_file_chain(&volume, first, clusters, &count)) return false;
    entry[0] = 0xe5;
    if (!write_sector(&volume, dir_lba, sector) ||
        !clear_fat_chain(&volume, clusters, count)) return false;
    return driver_flush(object_kernel_handles(), volume.write_cap);
}
