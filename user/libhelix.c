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

__attribute__((noreturn)) void helix_exit(int status) {
    long number = 0;
    __asm__ volatile("syscall"
                     : "+a"(number)
                     : "D"((long)status)
                     : "rcx", "r11", "memory", "cc");
    for (;;) __asm__ volatile("ud2");
}
