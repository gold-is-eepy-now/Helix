/* hfs.c — bounded HFS v1 root directory and contiguous-file backend. */
#include "hfs.h"

#include <stdint.h>

#include "console.h"
#include "driver.h"
#include "kstring.h"
#include "object.h"

#define HFS_SECTOR_SIZE 512u
#define HFS_ROOT_START 1u
#define HFS_ROOT_SECTORS 16u
#define HFS_RECORD_SIZE 64u
#define HFS_RECORD_COUNT 128u
#define HFS_DATA_START 17u
#define HFS_MAX_FILE_SIZE 8192u
#define HFS_DIRECTORY_BYTES (HFS_ROOT_SECTORS * HFS_SECTOR_SIZE)

typedef struct {
    handle_t read_cap;
    handle_t write_cap;
    uint64_t base;
    uint64_t sectors;
    uint64_t device_sectors;
} hfs_volume_t;

static uint16_t get16(const uint8_t *p) {
    return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
}

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get64(const uint8_t *p) {
    return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32);
}

static void put32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static void put64(uint8_t *p, uint64_t value) {
    put32(p, (uint32_t)value);
    put32(p + 4, (uint32_t)(value >> 32));
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t length) {
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return crc;
}

static uint32_t crc32(const uint8_t *data, size_t length) {
    return crc32_update(0xffffffffu, data, length) ^ 0xffffffffu;
}

static bool open_block_device(hfs_volume_t *v, const driver_info_t *info) {
    if (v == NULL || info == NULL ||
        strcmp(info->device_class, "block-storage") != 0 ||
        info->read_handle == 0 || info->write_handle == 0 ||
        info->sector_size != HFS_SECTOR_SIZE ||
        info->sector_count <= HFS_DATA_START) return false;
    memset(v, 0, sizeof(*v));
    v->read_cap = info->read_handle;
    v->write_cap = info->write_handle;
    v->sectors = info->sector_count;
    v->device_sectors = info->sector_count;
    return true;
}

static bool read_sector(const hfs_volume_t *v, uint64_t lba, uint8_t *out) {
    return lba < v->sectors && v->base + lba >= v->base &&
        v->base + lba < v->device_sectors &&
        driver_read(object_kernel_handles(), v->read_cap, v->base + lba,
                    out, HFS_SECTOR_SIZE);
}

static bool write_sector(const hfs_volume_t *v, uint64_t lba,
                         const uint8_t *data) {
    return lba < v->sectors && v->base + lba >= v->base &&
        v->base + lba < v->device_sectors &&
        driver_write(object_kernel_handles(), v->write_cap, v->base + lba,
                    data, HFS_SECTOR_SIZE);
}

static bool valid_superblock(const hfs_volume_t *v, const uint8_t *b) {
    static const uint8_t magic[8] = {'H','E','L','I','X','F','S','1'};
    if (memcmp(b, magic, sizeof(magic)) != 0 || get32(b + 8) != 1 ||
        get32(b + 12) != HFS_SECTOR_SIZE || get64(b + 16) > v->sectors ||
        get64(b + 16) <= HFS_DATA_START || get64(b + 24) != HFS_ROOT_START ||
        get32(b + 32) != HFS_ROOT_SECTORS ||
        get32(b + 36) != HFS_RECORD_SIZE || get32(b + 40) != 0 ||
        get32(b + 44) != crc32(b, 44)) return false;
    for (size_t i = 48; i < HFS_SECTOR_SIZE; i++) if (b[i] != 0) return false;
    return true;
}

static bool locate_hfs_volume(hfs_volume_t *v) {
    uint8_t sector[HFS_SECTOR_SIZE];
    if (read_sector(v, 0, sector) && valid_superblock(v, sector)) {
        v->sectors = get64(sector + 16);
        return true;
    }
    if (!read_sector(v, 1, sector) || memcmp(sector, "EFI PART", 8) != 0)
        return false;
    uint64_t entries_lba = get64(sector + 72);
    uint32_t entry_count = get32(sector + 80);
    uint32_t entry_size = get32(sector + 84);
    if (entry_count == 0 || entry_count > 128 || entry_size != 128 ||
        entries_lba >= v->sectors) return false;
    uint8_t entries_sector[HFS_SECTOR_SIZE];
    uint64_t cached_lba = UINT64_MAX;
    for (uint32_t i = 0; i < entry_count; i++) {
        uint64_t byte_offset = (uint64_t)i * entry_size;
        uint64_t table_lba = entries_lba + byte_offset / HFS_SECTOR_SIZE;
        if (table_lba >= v->sectors) return false;
        if (table_lba != cached_lba) {
            if (!read_sector(v, table_lba, entries_sector)) return false;
            cached_lba = table_lba;
        }
        const uint8_t *entry = entries_sector + byte_offset % HFS_SECTOR_SIZE;
        bool empty_type = true;
        for (uint32_t j = 0; j < 16; j++) if (entry[j] != 0) empty_type = false;
        if (empty_type) continue;
        uint64_t first = get64(entry + 32), last = get64(entry + 40);
        if (first == 0 || last < first || last >= v->sectors) continue;
        hfs_volume_t candidate = *v;
        candidate.base = first;
        candidate.sectors = last - first + 1;
        if (!read_sector(&candidate, 0, sector) ||
            !valid_superblock(&candidate, sector) ||
            get64(sector + 16) > candidate.sectors) continue;
        candidate.sectors = get64(sector + 16);
        *v = candidate;
        return true;
    }
    return false;
}

static bool validate_directory(const hfs_volume_t *v,
                               const uint8_t *directory) {
    for (uint32_t i = 0; i < HFS_RECORD_COUNT; i++) {
        const uint8_t *r = directory + i * HFS_RECORD_SIZE;
        if (r[0] == 0) {
            for (uint32_t j = 1; j < HFS_RECORD_SIZE; j++)
                if (r[j] != 0) return false;
            continue;
        }
        if (r[0] != 1 || r[1] == 0 || r[1] > 39 || get16(r + 2) != 0)
            return false;
        for (uint32_t j = r[1]; j < 40; j++) if (r[24 + j] != 0) return false;
        for (uint32_t j = 0; j < r[1]; j++)
            if (r[24 + j] < 0x20 || r[24 + j] == '/' || r[24 + j] == '\\')
                return false;
        for (uint32_t j = 0; j < 24; j++) {
            if (j == 0 || j == 1 || (j >= 4 && j < 24)) continue;
            if (r[j] != 0) return false;
        }
        uint64_t start = get64(r + 4), length = get64(r + 12);
        uint64_t count = (length + HFS_SECTOR_SIZE - 1) / HFS_SECTOR_SIZE;
        if (length == 0) {
            if (start != 0 || get32(r + 20) != 0) return false;
        } else if (length > HFS_MAX_FILE_SIZE || start < HFS_DATA_START ||
                   start > v->sectors || count > v->sectors - start)
            return false;

        for (uint32_t j = 0; j < i; j++) {
            const uint8_t *other = directory + j * HFS_RECORD_SIZE;
            if (other[0] != 1) continue;
            if (other[1] == r[1] && memcmp(other + 24, r + 24, r[1]) == 0)
                return false;
            uint64_t other_start = get64(other + 4);
            uint64_t other_len = get64(other + 12);
            uint64_t other_count = (other_len + HFS_SECTOR_SIZE - 1) /
                                   HFS_SECTOR_SIZE;
            if (count != 0 && other_count != 0 &&
                start < other_start + other_count &&
                other_start < start + count) return false;
        }
    }
    return true;
}

static bool mount_any(hfs_volume_t *v, uint8_t *directory) {
    driver_info_t info;
    for (size_t i = 0; i < driver_count(); i++) {
        hfs_volume_t candidate = {0};
        uint8_t superblock[HFS_SECTOR_SIZE];
        if (!driver_get_info(i, &info) || !open_block_device(&candidate, &info) ||
            !locate_hfs_volume(&candidate) ||
            !read_sector(&candidate, 0, superblock) ||
            !valid_superblock(&candidate, superblock)) continue;
        for (uint32_t s = 0; s < HFS_ROOT_SECTORS; s++)
            if (!read_sector(&candidate, HFS_ROOT_START + s,
                             directory + s * HFS_SECTOR_SIZE)) return false;
        if (!validate_directory(&candidate, directory)) continue;
        *v = candidate;
        return true;
    }
    return false;
}

/* Kept as an alias while existing callers migrate from the original usb0 API. */
static bool mount_usb0(hfs_volume_t *v, uint8_t *directory) {
    return mount_any(v, directory);
}

static bool normalize_path(const char *path, char name[40], size_t *length) {
    if (path == NULL || name == NULL || length == NULL) return false;
    if (path[0] == '/' ) path++;
    else if (path[0] == 'h' && path[1] == 'f' && path[2] == 's' &&
             path[3] == ':' && path[4] == '/') path += 5;
    size_t n = 0;
    while (path[n] != '\0') {
        if (n == 39 || path[n] == '/' || path[n] == '\\' ||
            (uint8_t)path[n] < 0x20) return false;
        name[n] = path[n];
        n++;
    }
    if (n == 0 || (n == 1 && name[0] == '.') ||
        (n == 2 && name[0] == '.' && name[1] == '.')) return false;
    name[n] = '\0';
    *length = n;
    return true;
}

bool hfs_usb0_is_formatted(void) {
    driver_info_t info;
    uint8_t sector[HFS_SECTOR_SIZE];
    for (size_t i = 0; i < driver_count(); i++) {
        hfs_volume_t volume = {0};
        if (driver_get_info(i, &info) && open_block_device(&volume, &info) &&
            locate_hfs_volume(&volume) && read_sector(&volume, 0, sector) &&
            valid_superblock(&volume, sector)) return true;
    }
    return false;
}

bool hfs_write_file(const char *path, const char *data,
                    unsigned long length) {
    if (path == NULL || (length != 0 && data == NULL) ||
        length > HFS_MAX_FILE_SIZE) return false;
    char name[40];
    size_t name_len = 0;
    if (!normalize_path(path, name, &name_len)) return false;

    hfs_volume_t volume = {0};
    uint8_t directory[HFS_DIRECTORY_BYTES];
    if (!mount_usb0(&volume, directory)) return false;
    uint32_t free_slot = HFS_RECORD_COUNT;
    uint32_t existing_slot = HFS_RECORD_COUNT;
    for (uint32_t i = 0; i < HFS_RECORD_COUNT; i++) {
        const uint8_t *r = directory + i * HFS_RECORD_SIZE;
        if (r[0] == 0 && free_slot == HFS_RECORD_COUNT) free_slot = i;
        if (r[0] == 1 && r[1] == name_len &&
            memcmp(r + 24, name, name_len) == 0) existing_slot = i;
    }
    if (existing_slot == HFS_RECORD_COUNT && free_slot == HFS_RECORD_COUNT)
        return false;
    uint32_t record_slot = existing_slot != HFS_RECORD_COUNT
        ? existing_slot : free_slot;

    uint64_t needed = (length + HFS_SECTOR_SIZE - 1) / HFS_SECTOR_SIZE;
    uint64_t start = HFS_DATA_START;
    if (needed != 0) {
        while (start + needed <= volume.sectors) {
            uint64_t next = start;
            bool collision = false;
            for (uint32_t i = 0; i < HFS_RECORD_COUNT; i++) {
                const uint8_t *r = directory + i * HFS_RECORD_SIZE;
                if (r[0] != 1) continue;
                uint64_t used_start = get64(r + 4);
                uint64_t used_count = (get64(r + 12) + HFS_SECTOR_SIZE - 1) /
                                      HFS_SECTOR_SIZE;
                if (used_count != 0 && start < used_start + used_count &&
                    used_start < start + needed) {
                    uint64_t after = used_start + used_count;
                    if (after > next) next = after;
                    collision = true;
                }
            }
            if (!collision) break;
            start = next;
        }
        if (start + needed > volume.sectors) return false;
    } else start = 0;

    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t file_crc = crc32(bytes, length);
    uint8_t sector[HFS_SECTOR_SIZE];
    size_t offset = 0;
    for (uint64_t i = 0; i < needed; i++) {
        memset(sector, 0, sizeof(sector));
        size_t amount = length - offset;
        if (amount > sizeof(sector)) amount = sizeof(sector);
        memcpy(sector, bytes + offset, amount);
        if (!write_sector(&volume, start + i, sector)) return false;
        offset += amount;
    }

    uint8_t *record = directory + record_slot * HFS_RECORD_SIZE;
    memset(record, 0, HFS_RECORD_SIZE);
    record[0] = 1;
    record[1] = (uint8_t)name_len;
    put64(record + 4, start);
    put64(record + 12, length);
    put32(record + 20, file_crc);
    memcpy(record + 24, name, name_len);
    uint64_t record_sector = HFS_ROOT_START +
        ((uint64_t)record_slot * HFS_RECORD_SIZE) / HFS_SECTOR_SIZE;
    uint32_t record_offset = (record_slot * HFS_RECORD_SIZE) % HFS_SECTOR_SIZE;
    if (!read_sector(&volume, record_sector, sector)) return false;
    memcpy(sector + record_offset, record, HFS_RECORD_SIZE);
    return write_sector(&volume, record_sector, sector);
}

bool hfs_save_usb0_new_file(const char *name, const char *data,
                            unsigned long length) {
    return hfs_write_file(name, data, length);
}

static bool verify_file_crc(const hfs_volume_t *v, const uint8_t *r);

bool hfs_read_file(const char *path, char *buffer, unsigned long capacity,
                   unsigned long *length_out) {
    char name[40];
    size_t name_len = 0;
    if (!normalize_path(path, name, &name_len) || buffer == NULL ||
        length_out == NULL) return false;
    hfs_volume_t volume = {0};
    uint8_t directory[HFS_DIRECTORY_BYTES];
    if (!mount_usb0(&volume, directory)) return false;
    const uint8_t *record = NULL;
    for (uint32_t i = 0; i < HFS_RECORD_COUNT; i++) {
        const uint8_t *candidate = directory + i * HFS_RECORD_SIZE;
        if (candidate[0] == 1 && candidate[1] == name_len &&
            memcmp(candidate + 24, name, name_len) == 0) {
            record = candidate;
            break;
        }
    }
    if (record == NULL || !verify_file_crc(&volume, record)) return false;
    uint64_t length = get64(record + 12);
    if (length > capacity) return false;
    uint64_t remaining = length;
    uint64_t lba = get64(record + 4);
    size_t offset = 0;
    uint8_t sector[HFS_SECTOR_SIZE];
    while (remaining != 0) {
        if (!read_sector(&volume, lba++, sector)) return false;
        size_t amount = remaining < sizeof(sector) ? (size_t)remaining
                                                    : sizeof(sector);
        memcpy(buffer + offset, sector, amount);
        offset += amount;
        remaining -= amount;
    }
    *length_out = (unsigned long)length;
    return true;
}

bool hfs_delete_file(const char *path) {
    char name[40];
    size_t name_len = 0;
    if (!normalize_path(path, name, &name_len)) return false;
    hfs_volume_t volume = {0};
    uint8_t directory[HFS_DIRECTORY_BYTES];
    if (!mount_usb0(&volume, directory)) return false;
    for (uint32_t i = 0; i < HFS_RECORD_COUNT; i++) {
        const uint8_t *record = directory + i * HFS_RECORD_SIZE;
        if (record[0] != 1 || record[1] != name_len ||
            memcmp(record + 24, name, name_len) != 0) continue;
        uint64_t lba = HFS_ROOT_START +
            ((uint64_t)i * HFS_RECORD_SIZE) / HFS_SECTOR_SIZE;
        uint32_t offset = (i * HFS_RECORD_SIZE) % HFS_SECTOR_SIZE;
        uint8_t sector[HFS_SECTOR_SIZE];
        if (!read_sector(&volume, lba, sector)) return false;
        memset(sector + offset, 0, HFS_RECORD_SIZE);
        return write_sector(&volume, lba, sector);
    }
    return false;
}

bool hfs_format_device(const char *device_name) {
    if (device_name == NULL || device_name[0] == '\0') return false;
    driver_info_t info;
    for (size_t i = 0; i < driver_count(); i++) {
        if (!driver_get_info(i, &info) ||
            strcmp(info.name, device_name) != 0 ||
            strcmp(info.device_class, "block-storage") != 0 ||
            info.read_handle == 0 || info.write_handle == 0 ||
            info.sector_size != HFS_SECTOR_SIZE ||
            info.sector_count <= HFS_DATA_START) continue;

        uint8_t sector[HFS_SECTOR_SIZE];
        bool blank = true;
        const uint64_t probes[] = {0, 1, info.sector_count - 1};
        for (size_t p = 0; p < sizeof(probes) / sizeof(probes[0]); p++) {
            if (!driver_read(object_kernel_handles(), info.read_handle,
                             probes[p], sector, sizeof(sector))) return false;
            for (size_t b = 0; b < sizeof(sector); b++)
                if (sector[b] != 0) blank = false;
        }
        if (!blank) {
            console_write("HFS format: refusing nonblank device ");
            console_write(device_name);
            console_write("; format is limited to blank disks.\n");
            return false;
        }

        memset(sector, 0, sizeof(sector));
        for (uint64_t lba = 1; lba <= HFS_ROOT_SECTORS; lba++) {
            if (!driver_write(object_kernel_handles(), info.write_handle,
                              lba, sector, sizeof(sector))) {
                console_write("HFS format: failed clearing root directory at LBA ");
                console_write_dec(lba);
                console_putc('\n');
                return false;
            }
        }

        static const uint8_t magic[8] = {'H','E','L','I','X','F','S','1'};
        memcpy(sector, magic, sizeof(magic));
        put32(sector + 8, 1);
        put32(sector + 12, HFS_SECTOR_SIZE);
        put64(sector + 16, info.sector_count);
        put64(sector + 24, HFS_ROOT_START);
        put32(sector + 32, HFS_ROOT_SECTORS);
        put32(sector + 36, HFS_RECORD_SIZE);
        put32(sector + 40, 0);
        put32(sector + 44, crc32(sector, 44));
        if (!driver_write(object_kernel_handles(), info.write_handle,
                          0, sector, sizeof(sector))) {
            console_write("HFS format: failed writing superblock.\n");
            return false;
        }
        console_write("HFS v1 formatted blank disk ");
        console_write(device_name);
        console_write(" (raw volume, ");
        console_write_dec(info.sector_count);
        console_write(" sectors).\n");
        return true;
    }
    console_write("HFS format: writable 512-byte block device not found: ");
    console_write(device_name);
    console_putc('\n');
    return false;
}

static bool verify_file_crc(const hfs_volume_t *v, const uint8_t *r) {
    uint64_t length = get64(r + 12);
    uint64_t lba = get64(r + 4);
    uint64_t remaining = length;
    uint32_t crc = 0xffffffffu;
    uint8_t sector[HFS_SECTOR_SIZE];
    while (remaining != 0) {
        if (!read_sector(v, lba++, sector)) return false;
        size_t amount = remaining < sizeof(sector) ? (size_t)remaining : sizeof(sector);
        crc = crc32_update(crc, sector, amount);
        remaining -= amount;
    }
    return (crc ^ 0xffffffffu) == get32(r + 20);
}

bool hfs_print_usb0_list(void) {
    hfs_volume_t volume = {0};
    uint8_t directory[HFS_DIRECTORY_BYTES];
    if (!mount_usb0(&volume, directory)) return false;
    console_write("HFS v1 root directory:\n");
    for (uint32_t i = 0; i < HFS_RECORD_COUNT; i++) {
        const uint8_t *r = directory + i * HFS_RECORD_SIZE;
        if (r[0] != 1) continue;
        if (!verify_file_crc(&volume, r)) {
            console_write("  HFS: file checksum failed: ");
            for (uint32_t j = 0; j < r[1]; j++) console_putc((char)r[24 + j]);
            console_putc('\n');
            return false;
        }
        console_write("  ");
        for (uint32_t j = 0; j < r[1]; j++) console_putc((char)r[24 + j]);
        console_write("  ");
        console_write_dec(get64(r + 12));
        console_write(" bytes\n");
    }
    return true;
}

bool hfs_print_usb0_file(const char *name) {
    char normalized[40];
    size_t name_len = 0;
    if (!normalize_path(name, normalized, &name_len)) return false;
    hfs_volume_t volume = {0};
    uint8_t directory[HFS_DIRECTORY_BYTES];
    if (!mount_usb0(&volume, directory)) return false;
    const uint8_t *record = NULL;
    for (uint32_t i = 0; i < HFS_RECORD_COUNT; i++) {
        const uint8_t *candidate = directory + i * HFS_RECORD_SIZE;
        if (candidate[0] == 1 && candidate[1] == name_len &&
            memcmp(candidate + 24, normalized, name_len) == 0) {
            record = candidate;
            break;
        }
    }
    if (record == NULL || !verify_file_crc(&volume, record)) return false;
    uint64_t length = get64(record + 12);
    uint64_t lba = get64(record + 4);
    uint8_t sector[HFS_SECTOR_SIZE];
    uint64_t remaining = length;
    console_write("--- ");
    console_write(normalized);
    console_write(" ---\n");
    while (remaining != 0) {
        if (!read_sector(&volume, lba++, sector)) return false;
        size_t amount = remaining < sizeof(sector) ? (size_t)remaining : sizeof(sector);
        for (size_t i = 0; i < amount; i++) {
            uint8_t c = sector[i];
            if (c == '\n' || c == '\r' || c == '\t' || c >= 0x20)
                console_putc((char)c);
            else console_putc('.');
        }
        remaining -= amount;
    }
    console_putc('\n');
    return true;
}
