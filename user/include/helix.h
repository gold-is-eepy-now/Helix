/* helix.h — minimal C interface for native Helix user programs. */
#ifndef HELIX_USER_HELIX_H
#define HELIX_USER_HELIX_H

typedef unsigned long helix_size_t;

long helix_write(const void *buffer, helix_size_t length);
long helix_read(void *buffer, helix_size_t capacity);
long helix_clear(void);
long helix_version(void);
long helix_sysinfo(void);
long helix_edit(void);
long helix_edit_file(const char *path);
long helix_drivers(void);
long helix_uart_capability(void);
long helix_uart_write_byte(unsigned int capability, unsigned int byte);
long helix_usb_info(void);
long helix_usb_service_channel(void);
long helix_usb_read_sector(unsigned int capability, unsigned long lba,
                           void *buffer);
long helix_usb_write_sector(unsigned int capability, unsigned long lba,
                            const void *buffer);
int helix_fat32_list(unsigned int start_cluster);
int helix_fat32_list_all(void);
int helix_fat32_cat(const char *path);
int helix_fat32_ls(void);
int helix_fat32_cd(const char *path);
int helix_fat32_pwd(void);
void helix_fat32_prompt(void);
long helix_hfs_list(void);
long helix_hfs_cat(const char *name);
long helix_hfs_read(const char *path, void *buffer, helix_size_t capacity);
long helix_hfs_write(const char *path, const void *buffer,
                     helix_size_t length);
long helix_hfs_delete(const char *path);
long helix_fat32_delete(const char *name);
long helix_fat32_write(const char *name, const void *data,
                       helix_size_t length);
long helix_hfs_format(const char *device_name);
long helix_install(const char *device_name);
__attribute__((noreturn)) void helix_exit(int status);

#endif /* HELIX_USER_HELIX_H */
