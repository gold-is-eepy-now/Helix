/* libhelix.c — tiny freestanding wrappers for the native Helix ABI. */
#include "helix.h"

long helix_write(const void *buffer, helix_size_t length) {
    long result = 1;
    __asm__ volatile("syscall"
                     : "+a"(result)
                     : "D"(buffer), "S"(length)
                     : "rcx", "r11", "memory", "cc");
    return result;
}

long helix_read(void *buffer, helix_size_t capacity) {
    long result = 2;
    __asm__ volatile("syscall"
                     : "+a"(result)
                     : "D"(buffer), "S"(capacity)
                     : "rcx", "r11", "memory", "cc");
    return result;
}

long helix_clear(void) {
    long result = 3;
    __asm__ volatile("syscall" : "+a"(result) :: "rcx", "r11", "memory", "cc");
    return result;
}

long helix_version(void) {
    long result = 20;
    __asm__ volatile("syscall" : "+a"(result) :: "rcx", "r11", "memory", "cc");
    return result;
}

long helix_sysinfo(void) {
    long result = 4;
    __asm__ volatile("syscall" : "+a"(result) :: "rcx", "r11", "memory", "cc");
    return result;
}

long helix_edit(void) {
    long result = 5;
    __asm__ volatile("syscall" : "+a"(result) : "D"(0UL)
                     : "rcx", "r11", "memory", "cc");
    return result;
}

long helix_edit_file(const char *path) {
    long result = 5;
    __asm__ volatile("syscall" : "+a"(result) : "D"(path)
                     : "rcx", "r11", "memory", "cc");
    return result;
}

long helix_drivers(void) {
    long result = 6;
    __asm__ volatile("syscall" : "+a"(result) :: "rcx", "r11", "memory", "cc");
    return result;
}

long helix_uart_capability(void) {
    long result = 8;
    __asm__ volatile("syscall" : "+a"(result) :: "rcx", "r11", "memory", "cc");
    return result;
}

long helix_uart_write_byte(unsigned int capability, unsigned int byte) {
    long result = 7;
    __asm__ volatile("syscall" : "+a"(result)
                     : "D"((unsigned long)capability), "S"((unsigned long)byte)
                     : "rcx", "r11", "memory", "cc");
    return result;
}

long helix_usb_info(void) {
    long result = 9;
    __asm__ volatile("syscall" : "+a"(result) :: "rcx", "r11", "memory", "cc");
    return result;
}

long helix_usb_service_channel(void) {
    long result = 10;
    __asm__ volatile("syscall" : "+a"(result) :: "rcx", "r11", "memory", "cc");
    return result;
}

long helix_usb_read_sector(unsigned int capability, unsigned long lba,
                           void *buffer) {
    long result = 11;
    __asm__ volatile("syscall" : "+a"(result)
                     : "D"((unsigned long)capability), "S"(lba), "d"(buffer)
                     : "rcx", "r11", "memory", "cc");
    return result;
}

long helix_usb_write_sector(unsigned int capability, unsigned long lba,
                            const void *buffer) {
    long result = 12;
    __asm__ volatile("syscall" : "+a"(result)
                     : "D"((unsigned long)capability), "S"(lba), "d"(buffer)
                     : "rcx", "r11", "memory", "cc");
    return result;
}

long helix_hfs_list(void) {
    long result = 13;
    __asm__ volatile("syscall" : "+a"(result) :: "rcx", "r11", "memory", "cc");
    return result;
}

long helix_hfs_cat(const char *name) {
    long result = 14;
    __asm__ volatile("syscall" : "+a"(result) : "D"(name)
                     : "rcx", "r11", "memory", "cc");
    return result;
}

long helix_hfs_read(const char *path, void *buffer, helix_size_t capacity) {
    long result = 16;
    __asm__ volatile("syscall" : "+a"(result)
                     : "D"(path), "S"(buffer), "d"(capacity)
                     : "rcx", "r11", "memory", "cc");
    return result;
}

long helix_hfs_write(const char *path, const void *buffer,
                     helix_size_t length) {
    long result = 17;
    __asm__ volatile("syscall" : "+a"(result)
                     : "D"(path), "S"(buffer), "d"(length)
                     : "rcx", "r11", "memory", "cc");
    return result;
}

long helix_hfs_delete(const char *path) {
    long result = 18;
    __asm__ volatile("syscall" : "+a"(result) : "D"(path)
                     : "rcx", "r11", "memory", "cc");
    return result;
}

long helix_fat32_delete(const char *name) {
    long result = 21;
    __asm__ volatile("syscall" : "+a"(result) : "D"(name)
                     : "rcx", "r11", "memory", "cc");
    return result;
}

long helix_fat32_write(const char *name, const void *data,
                       helix_size_t length) {
    long result = 22;
    __asm__ volatile("syscall" : "+a"(result)
                     : "D"(name), "S"(data), "d"(length)
                     : "rcx", "r11", "memory", "cc");
    return result;
}

long helix_hfs_format(const char *device_name) {
    long result = 19;
    __asm__ volatile("syscall" : "+a"(result) : "D"(device_name)
                     : "rcx", "r11", "memory", "cc");
    return result;
}

long helix_install(const char *device_name) {
    long result = 15;
    __asm__ volatile("syscall" : "+a"(result) : "D"(device_name)
                     : "rcx", "r11", "memory", "cc");
    return result;
}

__attribute__((noreturn)) void helix_exit(int status) {
    long number = 0;
    __asm__ volatile("syscall"
                     : "+a"(number)
                     : "D"((long)status)
                     : "rcx", "r11", "memory", "cc");
    for (;;) __asm__ volatile("ud2");
}
