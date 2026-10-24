/* hfs.h — first native Helix Filesystem v1 operations. */
#ifndef HELIX_HFS_H
#define HELIX_HFS_H

#include <stdbool.h>

bool hfs_usb0_is_formatted(void);
bool hfs_save_usb0_new_file(const char *name, const char *data, unsigned long length);
bool hfs_print_usb0_list(void);
bool hfs_print_usb0_file(const char *name);
bool hfs_read_file(const char *path, char *buffer, unsigned long capacity,
                   unsigned long *length_out);
bool hfs_write_file(const char *path, const char *data, unsigned long length);
bool hfs_delete_file(const char *path);
bool hfs_format_device(const char *device_name);

#endif /* HELIX_HFS_H */
