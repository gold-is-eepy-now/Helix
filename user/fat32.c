/* fat32.c — bounded, read-only FAT32 directory listing over the USB block API. */
#include "helix.h"

enum { SECTOR_BYTES = 512, MAX_DIR_CLUSTERS = 32, MAX_SECTORS_PER_CLUSTER = 128 };
enum { MAX_WALK_DIRS = 64, MAX_PATH_BYTES = 96 };
enum { MAX_CWD_DEPTH = 16 };

typedef struct {
    unsigned long partition_lba;
    unsigned long total_sectors;
    unsigned long fat_lba;
    unsigned long fat_sectors;
    unsigned long first_data_lba;
    unsigned long cluster_count;
    unsigned int sectors_per_cluster;
    unsigned int root_cluster;
    unsigned int channel;
} fat32_volume_t;

static unsigned int cwd_clusters[MAX_CWD_DEPTH];
static char cwd_names[MAX_CWD_DEPTH][13];
static unsigned int cwd_depth;
static int cwd_initialized;

static unsigned int read_le16(const unsigned char *p) {
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}

static unsigned long read_le32(const unsigned char *p) {
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
           ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

static void print(const char *s) {
    helix_size_t n = 0;
    while (s[n] != '\0') n++;
    (void)helix_write(s, n);
}

static void print_number(unsigned long value) {
    char digits[20];
    unsigned int count = 0;
    do {
        digits[count++] = (char)('0' + value % 10UL);
        value /= 10UL;
    } while (value != 0 && count < sizeof(digits));
    while (count != 0) {
        char c = digits[--count];
        (void)helix_write(&c, 1);
    }
}

static int read_sector(fat32_volume_t *volume, unsigned long lba,
                       unsigned char *sector) {
    return helix_usb_read_sector(volume->channel, lba, sector) == 0;
}

static int fat32_parse_boot(fat32_volume_t *volume, unsigned long base,
                            const unsigned char *boot) {
    unsigned int bytes_per_sector = read_le16(boot + 11);
    unsigned int sectors_per_cluster = boot[13];
    unsigned int reserved = read_le16(boot + 14);
    unsigned int fat_count = boot[16];
    unsigned int fat16_size = read_le16(boot + 22);
    unsigned long total = read_le16(boot + 19);
    if (total == 0) total = read_le32(boot + 32);
    unsigned long fat_size = read_le32(boot + 36);
    unsigned long root = read_le32(boot + 44) & 0x0fffffffUL;

    if (boot[510] != 0x55 || boot[511] != 0xaa ||
        bytes_per_sector != SECTOR_BYTES || sectors_per_cluster == 0 ||
        sectors_per_cluster > MAX_SECTORS_PER_CLUSTER ||
        (sectors_per_cluster & (sectors_per_cluster - 1u)) != 0 ||
        reserved == 0 || fat_count == 0 || fat_count > 2 ||
        fat16_size != 0 || fat_size == 0 || total == 0 || root < 2)
        return 0;

    unsigned long overhead = (unsigned long)reserved + fat_count * fat_size;
    if (overhead >= total) return 0;
    unsigned long cluster_count = (total - overhead) / sectors_per_cluster;
    if (cluster_count == 0 || cluster_count > 0x0fffffeeUL ||
        root > cluster_count + 1) return 0;

    volume->partition_lba = base;
    volume->total_sectors = total;
    volume->fat_lba = base + reserved;
    volume->fat_sectors = fat_size;
    volume->first_data_lba = base + overhead;
    volume->cluster_count = cluster_count;
    volume->sectors_per_cluster = sectors_per_cluster;
    volume->root_cluster = (unsigned int)root;
    return 1;
}

static int fat32_find_volume(fat32_volume_t *volume,
                             unsigned char *sector) {
    if (!read_sector(volume, 0, sector)) return 0;
    if (fat32_parse_boot(volume, 0, sector)) return 1;

    /* If LBA 0 is an MBR, try each partition entry that advertises FAT32. */
    for (unsigned int i = 0; i < 4; i++) {
        const unsigned char *entry = sector + 446 + i * 16;
        unsigned char type = entry[4];
        if (type != 0x0b && type != 0x0c && type != 0x1b && type != 0x1c)
            continue;
        unsigned long start = read_le32(entry + 8);
        if (start == 0 || !read_sector(volume, start, sector)) continue;
        if (fat32_parse_boot(volume, start, sector)) return 1;
    }
    return 0;
}

static int next_cluster(fat32_volume_t *volume, unsigned int cluster,
                        unsigned char *sector, unsigned int *next) {
    unsigned long byte_offset = (unsigned long)cluster * 4UL;
    unsigned long fat_sector = volume->fat_lba + byte_offset / SECTOR_BYTES;
    unsigned int offset = (unsigned int)(byte_offset % SECTOR_BYTES);
    if (fat_sector >= volume->fat_lba + volume->fat_sectors ||
        !read_sector(volume, fat_sector, sector)) return 0;
    *next = (unsigned int)(read_le32(sector + offset) & 0x0fffffffUL);
    return 1;
}

static void entry_name(char name[13], const unsigned char *entry) {
    unsigned int out = 0;
    for (unsigned int i = 0; i < 8 && entry[i] != ' '; i++)
        name[out++] = (char)entry[i];
    unsigned int extension = 0;
    for (unsigned int i = 8; i < 11; i++)
        if (entry[i] != ' ') extension++;
    if (extension != 0) {
        name[out++] = '.';
        for (unsigned int i = 8; i < 11 && entry[i] != ' '; i++)
            name[out++] = (char)entry[i];
    }
    name[out] = '\0';
}

static int is_dot_entry(const unsigned char *entry) {
    return entry[0] == '.' && (entry[1] == ' ' ||
        (entry[1] == '.' && entry[2] == ' '));
}

static int list_directory(fat32_volume_t *volume, unsigned int start_cluster) {
    unsigned int cluster = start_cluster;
    unsigned int seen[MAX_DIR_CLUSTERS];
    unsigned char sector[SECTOR_BYTES];
    int found_end = 0;

    print("read-only FAT32 directory: cluster ");
    print_number(cluster);
    print("\n");

    for (unsigned int chain = 0; chain < MAX_DIR_CLUSTERS; chain++) {
        if (cluster < 2 || cluster > volume->cluster_count + 1) break;
        for (unsigned int i = 0; i < chain; i++)
            if (seen[i] == cluster) {
                print("FAT32 error: directory cluster loop detected\n");
                return 0;
            }
        seen[chain] = cluster;
        unsigned long first = volume->first_data_lba +
            (unsigned long)(cluster - 2) * volume->sectors_per_cluster;
        for (unsigned int sec = 0; sec < volume->sectors_per_cluster; sec++) {
            if (!read_sector(volume, first + sec, sector)) {
                print("FAT32 error: directory sector read failed\n");
                return 0;
            }
            for (unsigned int offset = 0; offset < SECTOR_BYTES; offset += 32) {
                const unsigned char *entry = sector + offset;
                if (entry[0] == 0) { found_end = 1; break; }
                if (entry[0] == 0xe5 || entry[11] == 0x0f ||
                    (entry[11] & 0x08) != 0 || is_dot_entry(entry)) continue;
                unsigned int child = ((unsigned int)read_le16(entry + 20) << 16) |
                                     read_le16(entry + 26);
                char name[13];
                entry_name(name, entry);
                print((entry[11] & 0x10) ? "DIR  " : "FILE ");
                print(name);
                if ((entry[11] & 0x10) != 0) {
                    print("  cluster=");
                    print_number(child);
                } else {
                    print("  bytes=");
                    print_number(read_le32(entry + 28));
                }
                print("\n");
            }
            if (found_end) break;
        }
        if (found_end) break;
        unsigned int next = 0;
        if (!next_cluster(volume, cluster, sector, &next)) {
            print("FAT32 error: FAT chain read failed\n");
            return 0;
        }
        if (next >= 0x0ffffff8u || next == 0 || next == 0x0ffffff7u) break;
        cluster = next;
    }
    return 1;
}

typedef struct { unsigned int cluster; char path[MAX_PATH_BYTES]; } walk_dir_t;

static int list_all_directories(fat32_volume_t *volume) {
    walk_dir_t queue[MAX_WALK_DIRS];
    unsigned int queued = 1, current = 0;
    queue[0].cluster = volume->root_cluster;
    queue[0].path[0] = '\0';
    unsigned char sector[SECTOR_BYTES];
    int truncated = 0;

    while (current < queued) {
        unsigned int cluster = queue[current].cluster;
        print("directory ");
        print(queue[current].path[0] ? queue[current].path : "usb0:/");
        print("\n");
        unsigned int seen[MAX_DIR_CLUSTERS];
        unsigned int chain = 0;
        int found_end = 0;
        while (chain < MAX_DIR_CLUSTERS && cluster >= 2 &&
               cluster <= volume->cluster_count + 1) {
            int loop = 0;
            for (unsigned int i = 0; i < chain; i++)
                if (seen[i] == cluster) loop = 1;
            if (loop) {
                print("FAT32 warning: directory chain loop at cluster ");
                print_number(cluster);
                print("\n");
                break;
            }
            seen[chain++] = cluster;
            unsigned long first = volume->first_data_lba +
                (unsigned long)(cluster - 2) * volume->sectors_per_cluster;
            for (unsigned int sec = 0; sec < volume->sectors_per_cluster; sec++) {
                if (!read_sector(volume, first + sec, sector)) {
                    print("FAT32 error: directory sector read failed\n");
                    return 0;
                }
                for (unsigned int offset = 0; offset < SECTOR_BYTES; offset += 32) {
                    const unsigned char *entry = sector + offset;
                    if (entry[0] == 0) { found_end = 1; break; }
                    if (entry[0] == 0xe5 || entry[11] == 0x0f ||
                        (entry[11] & 0x08) != 0 || is_dot_entry(entry)) continue;
                    char name[13];
                    entry_name(name, entry);
                    print(queue[current].path);
                    print(name);
                    if ((entry[11] & 0x10) != 0) {
                        unsigned int child =
                            ((unsigned int)read_le16(entry + 20) << 16) |
                            read_le16(entry + 26);
                        print("/ [DIR cluster="); print_number(child); print("]\n");
                        if (child < 2 || child > volume->cluster_count + 1) continue;
                        int duplicate = 0;
                        for (unsigned int i = 0; i < queued; i++)
                            if (queue[i].cluster == child) duplicate = 1;
                        if (duplicate) continue;
                        if (queued == MAX_WALK_DIRS) { truncated = 1; continue; }
                        unsigned int path_len = 0;
                        while (queue[current].path[path_len] != '\0' &&
                               path_len + 1 < MAX_PATH_BYTES) path_len++;
                        unsigned int name_len = 0;
                        while (name[name_len] != '\0') name_len++;
                        if (path_len + name_len + 1 >= MAX_PATH_BYTES) {
                            truncated = 1;
                            continue;
                        }
                        for (unsigned int i = 0; i < path_len; i++)
                            queue[queued].path[i] = queue[current].path[i];
                        for (unsigned int i = 0; i < name_len; i++)
                            queue[queued].path[path_len + i] = name[i];
                        queue[queued].path[path_len + name_len] = '/';
                        queue[queued].path[path_len + name_len + 1] = '\0';
                        queue[queued++].cluster = child;
                    } else {
                        print("  bytes="); print_number(read_le32(entry + 28)); print("\n");
                    }
                }
                if (found_end) break;
            }
            if (found_end) break;
            unsigned int next = 0;
            if (!next_cluster(volume, cluster, sector, &next)) {
                print("FAT32 error: FAT chain read failed\n");
                return 0;
            }
            if (next >= 0x0ffffff8u || next == 0 || next == 0x0ffffff7u) break;
            cluster = next;
        }
        current++;
    }
    if (truncated) print("FAT32 listing stopped at its safe traversal limit.\n");
    return 1;
}

static int make_short_name(const char *path, unsigned char name[11]) {
    unsigned int base = 0, extension = 0;
    int after_dot = 0;
    for (unsigned int i = 0; i < 11; i++) name[i] = ' ';
    if (path[0] == 'u' && path[1] == 's' && path[2] == 'b' &&
        path[3] == '0' && path[4] == ':' && path[5] == '/') path += 6;
    if (*path == '\0') return 0;
    while (*path != '\0') {
        char c = *path++;
        if (c == '.') {
            if (after_dot || base == 0) return 0;
            after_dot = 1;
            continue;
        }
        if (c == '/' || c == '\\') return 0;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-' || c == '$' || c == '~')) return 0;
        if (!after_dot) {
            if (base == 8) return 0;
            name[base++] = (unsigned char)c;
        } else {
            if (extension == 3) return 0;
            name[8 + extension++] = (unsigned char)c;
        }
    }
    return 1;
}

static void cwd_initialize(const fat32_volume_t *volume) {
    if (cwd_initialized && cwd_clusters[0] == volume->root_cluster) return;
    cwd_clusters[0] = volume->root_cluster;
    cwd_depth = 0;
    cwd_initialized = 1;
}

static int find_child_directory(fat32_volume_t *volume,
                                unsigned int parent,
                                const unsigned char wanted[11],
                                unsigned int *child,
                                char display_name[13]) {
    unsigned char sector[SECTOR_BYTES];
    unsigned int seen[MAX_DIR_CLUSTERS], count = 0;
    while (count < MAX_DIR_CLUSTERS && parent >= 2 &&
           parent <= volume->cluster_count + 1) {
        for (unsigned int i = 0; i < count; i++)
            if (seen[i] == parent) return 0;
        seen[count++] = parent;
        unsigned long first = volume->first_data_lba +
            (unsigned long)(parent - 2) * volume->sectors_per_cluster;
        for (unsigned int sec = 0; sec < volume->sectors_per_cluster; sec++) {
            if (!read_sector(volume, first + sec, sector)) return 0;
            for (unsigned int off = 0; off < SECTOR_BYTES; off += 32) {
                const unsigned char *entry = sector + off;
                if (entry[0] == 0) return 0;
                if (entry[0] == 0xe5 || entry[11] == 0x0f ||
                    (entry[11] & 0x10) == 0 || is_dot_entry(entry)) continue;
                unsigned int i = 0;
                while (i < 11 && entry[i] == wanted[i]) i++;
                if (i != 11) continue;
                unsigned int value = ((unsigned int)read_le16(entry + 20) << 16) |
                                     read_le16(entry + 26);
                if (value < 2 || value > volume->cluster_count + 1) return 0;
                *child = value;
                entry_name(display_name, entry);
                return 1;
            }
        }
        unsigned int next = 0;
        if (!next_cluster(volume, parent, sector, &next) ||
            next < 2 || next > volume->cluster_count + 1 ||
            next >= 0x0ffffff8u) return 0;
        parent = next;
    }
    return 0;
}

int helix_fat32_ls(void) {
    long channel = helix_usb_service_channel();
    if (channel < 0) { print("USB storage service is unavailable.\n"); return 0; }
    fat32_volume_t volume = { .channel = (unsigned int)channel };
    unsigned char boot[SECTOR_BYTES];
    if (!fat32_find_volume(&volume, boot)) {
        print("No supported FAT32 volume found.\n"); return 0;
    }
    cwd_initialize(&volume);
    (void)helix_fat32_pwd();
    return list_directory(&volume, cwd_clusters[cwd_depth]);
}

int helix_fat32_cd(const char *path) {
    long channel = helix_usb_service_channel();
    if (channel < 0) { print("USB storage service is unavailable.\n"); return 0; }
    fat32_volume_t volume = { .channel = (unsigned int)channel };
    unsigned char boot[SECTOR_BYTES];
    if (!fat32_find_volume(&volume, boot)) {
        print("No supported FAT32 volume found.\n"); return 0;
    }
    cwd_initialize(&volume);
    if (path == 0 || path[0] == '\0' || path[0] == '/') {
        cwd_depth = 0;
        return 1;
    }
    if (path[0] == 'u' && path[1] == 's' && path[2] == 'b' &&
        path[3] == '0' && path[4] == ':' && path[5] == '/') {
        cwd_depth = 0;
        path += 6;
        if (*path == '\0') return 1;
    }
    if (path[0] == '.' && path[1] == '.' && path[2] == '\0') {
        if (cwd_depth != 0) cwd_depth--;
        return 1;
    }
    unsigned char wanted[11];
    if (!make_short_name(path, wanted)) {
        print("cd supports one short 8.3 directory name at a time.\n");
        return 0;
    }
    if (cwd_depth + 1 >= MAX_CWD_DEPTH) {
        print("maximum directory depth reached.\n"); return 0;
    }
    unsigned int child = 0;
    char display_name[13];
    if (!find_child_directory(&volume, cwd_clusters[cwd_depth], wanted,
                              &child, display_name)) {
        print("directory not found in current FAT32 directory.\n");
        return 0;
    }
    cwd_depth++;
    cwd_clusters[cwd_depth] = child;
    unsigned int i = 0;
    while (display_name[i] != '\0' && i < sizeof(cwd_names[0]) - 1) {
        cwd_names[cwd_depth][i] = display_name[i]; i++;
    }
    cwd_names[cwd_depth][i] = '\0';
    return 1;
}

int helix_fat32_pwd(void) {
    print("usb0:/");
    for (unsigned int depth = 1; depth <= cwd_depth; depth++) {
        print(cwd_names[depth]); print("/");
    }
    print("\n");
    return 1;
}

void helix_fat32_prompt(void) {
    print("user@helix:usb0:/");
    for (unsigned int depth = 1; depth <= cwd_depth; depth++) {
        print(cwd_names[depth]); print("/");
    }
    print("$ ");
}

static int cat_root_file(fat32_volume_t *volume, const char *path) {
    unsigned char wanted[11], sector[SECTOR_BYTES];
    if (!make_short_name(path, wanted)) {
        print("usage: usb cat [usb0:/]NAME.TXT (short 8.3 name, root directory)\n");
        return 0;
    }
    unsigned int directory = volume->root_cluster;
    unsigned int seen[MAX_DIR_CLUSTERS];
    unsigned int cluster_count = 0, file_cluster = 0, file_size = 0;
    int found = 0, ended = 0;
    while (!ended && cluster_count < MAX_DIR_CLUSTERS &&
           directory >= 2 && directory <= volume->cluster_count + 1) {
        for (unsigned int i = 0; i < cluster_count; i++)
            if (seen[i] == directory) {
                print("FAT32 error: root directory cluster loop detected\n");
                return 0;
            }
        seen[cluster_count++] = directory;
        unsigned long first = volume->first_data_lba +
            (unsigned long)(directory - 2) * volume->sectors_per_cluster;
        for (unsigned int sec = 0; sec < volume->sectors_per_cluster && !ended; sec++) {
            if (!read_sector(volume, first + sec, sector)) return 0;
            for (unsigned int off = 0; off < SECTOR_BYTES; off += 32) {
                const unsigned char *entry = sector + off;
                if (entry[0] == 0) { ended = 1; break; }
                if (entry[0] == 0xe5 || entry[11] == 0x0f ||
                    (entry[11] & 0x18) != 0) continue;
                unsigned int i = 0;
                while (i < 11 && entry[i] == wanted[i]) i++;
                if (i == 11) {
                    file_cluster = ((unsigned int)read_le16(entry + 20) << 16) |
                                   read_le16(entry + 26);
                    file_size = (unsigned int)read_le32(entry + 28);
                    found = 1; ended = 1; break;
                }
            }
        }
        if (ended) break;
        unsigned int next = 0;
        if (!next_cluster(volume, directory, sector, &next)) return 0;
        if (next >= 0x0ffffff8u) break;
        directory = next;
    }
    if (!found) { print("FAT32 file not found in the root directory.\n"); return 0; }
    if (file_size > 8192u) { print("FAT32 cat is limited to 8192 bytes.\n"); return 0; }
    if (file_size == 0) { print("\n"); return 1; }
    if (file_cluster < 2 || file_cluster > volume->cluster_count + 1) return 0;
    unsigned int file_seen[MAX_DIR_CLUSTERS];
    unsigned int file_seen_count = 0;
    unsigned int remaining = file_size;
    while (remaining != 0) {
        if (file_seen_count == MAX_DIR_CLUSTERS) return 0;
        for (unsigned int i = 0; i < file_seen_count; i++)
            if (file_seen[i] == file_cluster) return 0;
        file_seen[file_seen_count++] = file_cluster;
        unsigned long first = volume->first_data_lba +
            (unsigned long)(file_cluster - 2) * volume->sectors_per_cluster;
        for (unsigned int sec = 0; sec < volume->sectors_per_cluster && remaining != 0; sec++) {
            if (!read_sector(volume, first + sec, sector)) return 0;
            unsigned int amount = remaining < SECTOR_BYTES ? remaining : SECTOR_BYTES;
            (void)helix_write(sector, amount);
            remaining -= amount;
        }
        if (remaining == 0) break;
        unsigned int next = 0;
        if (!next_cluster(volume, file_cluster, sector, &next) ||
            next < 2 || next > volume->cluster_count + 1 || next >= 0x0ffffff8u)
            return 0;
        file_cluster = next;
    }
    print("\n");
    return 1;
}

int helix_fat32_list(unsigned int start_cluster) {
    long channel = helix_usb_service_channel();
    if (channel < 0) {
        print("USB storage service is unavailable.\n");
        return 0;
    }

    fat32_volume_t volume = { .channel = (unsigned int)channel };
    unsigned char boot[SECTOR_BYTES];
    if (!fat32_find_volume(&volume, boot)) {
        print("No supported FAT32 volume found at LBA 0 or an MBR partition.\n");
        return 0;
    }

    print("FAT32: ");
    print_number(volume.total_sectors);
    print(" sectors, ");
    print_number(volume.sectors_per_cluster);
    print(" sectors/cluster, root cluster ");
    print_number(volume.root_cluster);
    print("\n");
    return list_directory(&volume,
        start_cluster == 0 ? volume.root_cluster : start_cluster);
}

int helix_fat32_list_all(void) {
    long channel = helix_usb_service_channel();
    if (channel < 0) {
        print("USB storage service is unavailable.\n");
        return 0;
    }
    fat32_volume_t volume = { .channel = (unsigned int)channel };
    unsigned char boot[SECTOR_BYTES];
    if (!fat32_find_volume(&volume, boot)) {
        print("No supported FAT32 volume found at LBA 0 or an MBR partition.\n");
        return 0;
    }
    print("FAT32 recursive listing (short 8.3 names, read-only)\n");
    return list_all_directories(&volume);
}

int helix_fat32_cat(const char *path) {
    long channel = helix_usb_service_channel();
    if (channel < 0) { print("USB storage service is unavailable.\n"); return 0; }
    fat32_volume_t volume = { .channel = (unsigned int)channel };
    unsigned char boot[SECTOR_BYTES];
    if (!fat32_find_volume(&volume, boot)) {
        print("No supported FAT32 volume found at LBA 0 or an MBR partition.\n");
        return 0;
    }
    return cat_root_file(&volume, path);
}
