/* usermem.c — checked user-memory access (see usermem.h).
 *
 * The check reads the page-table entry for each page in the range: it must be
 * present, user-accessible, and writable when the kernel will write to it. We
 * then access the bytes at their user virtual address, which is valid because
 * the current address space is the caller's. */
#include "usermem.h"

#include "vmm.h"

/* Lowest address above the user half. User pointers must stay below it. */
#define USER_VA_LIMIT 0x0000800000000000ULL

bool usermem_valid(uint64_t addr, uint64_t len, bool write) {
    if (len == 0) return true;
    if (addr >= USER_VA_LIMIT || len > USER_VA_LIMIT - addr) return false;

    uint64_t end = addr + len;
    for (uint64_t page = addr & ~0xFFFULL; page < end; page += 4096) {
        uint64_t flags = 0;
        if (!vmm_get_mapping(page, NULL, &flags)) return false;
        if (!(flags & VMM_PRESENT) || !(flags & VMM_USER)) return false;
        if (write && !(flags & VMM_WRITE)) return false;
    }
    return true;
}

bool usermem_copy_in(void *dst, uint64_t user_src, uint64_t len) {
    if (!usermem_valid(user_src, len, false)) return false;
    const uint8_t *s = (const uint8_t *)(uintptr_t)user_src;
    uint8_t *d = dst;
    for (uint64_t i = 0; i < len; i++) d[i] = s[i];
    return true;
}

bool usermem_copy_out(uint64_t user_dst, const void *src, uint64_t len) {
    if (!usermem_valid(user_dst, len, true)) return false;
    uint8_t *d = (uint8_t *)(uintptr_t)user_dst;
    const uint8_t *s = src;
    for (uint64_t i = 0; i < len; i++) d[i] = s[i];
    return true;
}

bool usermem_copy_str(char *dst, size_t cap, uint64_t user_src) {
    if (cap == 0) return false;
    for (size_t i = 0; i < cap; i++) {
        if (!usermem_valid(user_src + i, 1, false)) return false;
        char c = *(const char *)(uintptr_t)(user_src + i);
        dst[i] = c;
        if (c == '\0') return true;
    }
    dst[cap - 1] = '\0';   /* no terminator within the limit */
    return false;
}
