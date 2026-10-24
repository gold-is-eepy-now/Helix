/* install.c — installs the bundled GPT/EFI/HFS disk image to a named disk. */
#include "install.h"

#include <stdint.h>

#include "console.h"
#include "driver.h"
#include "kstring.h"
#include "object.h"

#define INSTALL_SECTOR_SIZE 512u
#define INSTALL_CHUNK_SECTORS 64u
#define INSTALL_PROGRESS_SECTORS 16384u /* print every 8 MiB */
#define GPT_HEADER_MIN_SIZE 92u

static const uint8_t *g_install_image;
static size_t g_install_size;

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

static bool valid_gpt_header(const uint8_t *header, uint64_t current_lba,
                             uint64_t backup_lba) {
    uint32_t header_size = get32(header + 12);
    if (memcmp(header, "EFI PART", 8) != 0 ||
        header_size < GPT_HEADER_MIN_SIZE || header_size > INSTALL_SECTOR_SIZE ||
        get64(header + 24) != current_lba ||
        get64(header + 32) != backup_lba) return false;
    uint8_t copy[INSTALL_SECTOR_SIZE];
    memcpy(copy, header, sizeof(copy));
    uint32_t expected_crc = get32(copy + 16);
    put32(copy + 16, 0);
    return crc32(copy, header_size) == expected_crc;
}

bool install_image_set(const void *image, size_t size) {
    const uint8_t *bytes = image;
    if (bytes == NULL || size < 3u * INSTALL_SECTOR_SIZE ||
        size % INSTALL_SECTOR_SIZE != 0 ||
        memcmp(bytes + INSTALL_SECTOR_SIZE, "EFI PART", 8) != 0 ||
        bytes[510] != 0x55 || bytes[511] != 0xaa) return false;
    g_install_image = bytes;
    g_install_size = size;
    return true;
}

static bool update_gpt_for_target(handle_t write_cap, uint64_t target_sectors,
                                  uint64_t source_sectors) {
    uint8_t primary[INSTALL_SECTOR_SIZE];
    memcpy(primary, g_install_image + INSTALL_SECTOR_SIZE, sizeof(primary));
    uint32_t header_size = get32(primary + 12);
    uint64_t entries_lba = get64(primary + 72);
    uint32_t entry_count = get32(primary + 80);
    uint32_t entry_size = get32(primary + 84);
    if (header_size < GPT_HEADER_MIN_SIZE || header_size > INSTALL_SECTOR_SIZE ||
        entry_count == 0 || entry_count > 128 || entry_size < 128 ||
        entry_size > 512 || (entry_size & 7u) != 0) {
        console_write("Installer: bundled GPT geometry is invalid.\n");
        return false;
    }
    uint64_t entries_bytes = (uint64_t)entry_count * entry_size;
    uint64_t entries_sectors = (entries_bytes + INSTALL_SECTOR_SIZE - 1) /
                               INSTALL_SECTOR_SIZE;
    if (entries_lba > source_sectors ||
        entries_sectors > source_sectors - entries_lba ||
        target_sectors <= entries_sectors + 2) {
        console_write("Installer: GPT partition table does not fit target.\n");
        return false;
    }

    uint64_t last_lba = target_sectors - 1;
    uint64_t backup_entries_lba = target_sectors - entries_sectors - 1;
    uint64_t old_backup_lba = get64(primary + 32);
    uint64_t old_backup_entries = old_backup_lba >= entries_sectors
        ? old_backup_lba - entries_sectors : source_sectors;
    uint8_t sector[INSTALL_SECTOR_SIZE] = {0};

    /* Remove the image-sized backup GPT at the tail of the template before
     * placing a relocated backup at the real end of a larger VDI. */
    if (old_backup_entries < source_sectors) {
        for (uint64_t lba = old_backup_entries; lba < source_sectors; lba++) {
            if (!driver_write(object_kernel_handles(), write_cap, lba,
                              sector, sizeof(sector))) {
                console_write("Installer: could not clear template backup GPT.\n");
                return false;
            }
        }
    }

    uint32_t array_crc = 0xffffffffu;
    uint64_t remaining = entries_bytes;
    for (uint64_t i = 0; i < entries_sectors; i++) {
        const uint8_t *source = g_install_image +
            (entries_lba + i) * INSTALL_SECTOR_SIZE;
        size_t amount = remaining < INSTALL_SECTOR_SIZE
                      ? (size_t)remaining : INSTALL_SECTOR_SIZE;
        array_crc = crc32_update(array_crc, source, amount);
        memset(sector, 0, sizeof(sector));
        memcpy(sector, source, amount);
        if (!driver_write(object_kernel_handles(), write_cap,
                          backup_entries_lba + i, sector, sizeof(sector))) {
            console_write("Installer: failed writing backup GPT entries.\n");
            return false;
        }
        remaining -= amount;
    }
    array_crc ^= 0xffffffffu;

    put64(primary + 32, last_lba);
    put64(primary + 48, backup_entries_lba - 1);
    put32(primary + 16, 0);
    put32(primary + 16, crc32(primary, header_size));

    /* Expand the protective MBR length to cover the full VDI. */
    memcpy(sector, g_install_image, sizeof(sector));
    if (sector[450] == 0xee) {
        uint64_t protected_sectors = target_sectors - 1;
        if (protected_sectors > UINT32_MAX) protected_sectors = UINT32_MAX;
        put32(sector + 454, 1);
        put32(sector + 458, (uint32_t)protected_sectors);
        if (!driver_write(object_kernel_handles(), write_cap, 0,
                          sector, sizeof(sector))) {
            console_write("Installer: failed updating protective MBR.\n");
            return false;
        }
    }
    if (!driver_write(object_kernel_handles(), write_cap, 1,
                      primary, sizeof(primary))) {
        console_write("Installer: failed updating primary GPT header.\n");
        return false;
    }

    uint8_t backup[INSTALL_SECTOR_SIZE];
    memcpy(backup, primary, sizeof(backup));
    put64(backup + 24, last_lba);
    put64(backup + 32, 1);
    put64(backup + 72, backup_entries_lba);
    put32(backup + 88, array_crc);
    put32(backup + 16, 0);
    put32(backup + 16, crc32(backup, header_size));
    if (!driver_write(object_kernel_handles(), write_cap, last_lba,
                      backup, sizeof(backup))) {
        console_write("Installer: failed writing backup GPT header.\n");
        return false;
    }
    return true;
}

bool install_to_disk(const char *device_name) {
    if (device_name == NULL || device_name[0] == '\0' ||
        g_install_image == NULL || g_install_size == 0) {
        console_write("Installer: target or boot image is unavailable.\n");
        return false;
    }
    uint64_t source_sectors = g_install_size / INSTALL_SECTOR_SIZE;
    driver_info_t target = {0}, info;
    bool found = false;
    for (size_t i = 0; i < driver_count(); i++) {
        if (!driver_get_info(i, &info) || strcmp(info.name, device_name) != 0)
            continue;
        if (strcmp(info.device_class, "block-storage") != 0 ||
            info.read_handle == 0 || info.write_handle == 0 ||
            info.sector_size != INSTALL_SECTOR_SIZE) {
            console_write("Installer: selected device is not a writable 512-byte block disk.\n");
            return false;
        }
        if (info.sector_count < source_sectors) {
            console_write("Installer: selected disk is too small; needs at least ");
            console_write_dec(source_sectors);
            console_write(" sectors.\n");
            return false;
        }
        target = info;
        found = true;
        break;
    }
    if (!found) {
        console_write("Installer: block device not found: ");
        console_write(device_name);
        console_putc('\n');
        return false;
    }

    handle_t write_cap = target.write_handle;
    console_write("Installer: overwriting ");
    console_write(target.name);
    console_write(" (first ");
    console_write_dec(source_sectors);
    console_write(" sectors) with GPT, FAT32 EFI, and HFS.\n");

    /* Write the image in chunks. One driver call per sector costs three USB
     * bulk transfers each; 64 sectors (32 KiB) per call cuts that ~64x. Every
     * sector is still written (zero sectors included) so old data on the
     * target is overwritten rather than left behind. */
    for (uint64_t lba = 0; lba < source_sectors;) {
        uint64_t count = source_sectors - lba;
        if (count > INSTALL_CHUNK_SECTORS) count = INSTALL_CHUNK_SECTORS;
        if (!driver_write(object_kernel_handles(), write_cap, lba,
                          g_install_image + lba * INSTALL_SECTOR_SIZE,
                          (size_t)(count * INSTALL_SECTOR_SIZE))) {
            console_write("Installer: disk write failed at LBA ");
            console_write_dec(lba);
            console_putc('\n');
            return false;
        }
        uint64_t before = lba;
        lba += count;
        if ((before / INSTALL_PROGRESS_SECTORS) != (lba / INSTALL_PROGRESS_SECTORS)) {
            console_write("Installer: wrote through LBA ");
            console_write_dec(lba - 1u);
            console_putc('\n');
        }
    }

    if (target.sector_count > source_sectors &&
        !update_gpt_for_target(write_cap, target.sector_count, source_sectors))
        return false;

    /* Commit the device's volatile write cache before we call it installed. */
    if (!driver_flush(object_kernel_handles(), write_cap)) {
        console_write("Installer: warning: device did not confirm a cache flush; "
                      "do not remove it until it is idle.\n");
    }

    uint64_t last_lba = target.sector_count - 1;
    uint8_t sector[INSTALL_SECTOR_SIZE];
    uint8_t primary[INSTALL_SECTOR_SIZE], backup[INSTALL_SECTOR_SIZE];
    if (!driver_read(object_kernel_handles(), target.read_handle, 0,
                     sector, sizeof(sector)) || sector[510] != 0x55 ||
        sector[511] != 0xaa || sector[450] != 0xee ||
        !driver_read(object_kernel_handles(), target.read_handle, 1,
                     primary, sizeof(primary)) ||
        !valid_gpt_header(primary, 1, last_lba) ||
        !driver_read(object_kernel_handles(), target.read_handle, last_lba,
                     backup, sizeof(backup)) ||
        !valid_gpt_header(backup, last_lba, 1)) {
        console_write("Installer: GPT read-back verification failed.\n");
        return false;
    }
    if (65536 >= source_sectors ||
        !driver_read(object_kernel_handles(), target.read_handle, 65536,
                     sector, sizeof(sector)) ||
        memcmp(sector, g_install_image + 65536u * INSTALL_SECTOR_SIZE,
               sizeof(sector)) != 0) {
        console_write("Installer: HFS superblock read-back verification failed.\n");
        return false;
    }
    console_write("Installer: Helix with HFS installed and verified.\n");
    console_write("Installer: shut down and boot from the target disk.\n");
    return true;
}
