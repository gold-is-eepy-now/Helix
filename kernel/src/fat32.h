/* fat32.h — bounded FAT32 text-file creation helpers. */
#ifndef HELIX_FAT32_H
#define HELIX_FAT32_H

#include <stdbool.h>
#include <stddef.h>

bool fat32_save_usb0_file(const char *name, const char *data, size_t length);
bool fat32_delete_usb0_file(const char *name);

#endif /* HELIX_FAT32_H */
