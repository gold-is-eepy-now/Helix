/* Cell runtime: system call wrappers. Numbers and error codes match kernel/src/syscall.c. */
#ifndef LOOM_ULIB_H
#define LOOM_ULIB_H

#include <stdint.h>
#include <stddef.h>

#define SYS_EXIT   1
#define SYS_SEND   2
#define SYS_RECV   3
#define SYS_LOG    4
#define SYS_YIELD  5

struct msg {
    uint32_t type;
    uint8_t  payload[32];
};

static inline int64_t sys3(int64_t num, uint64_t a, uint64_t b, uint64_t c) {
    int64_t ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(num), "D"(a), "S"(b), "d"(c)
                      : "memory");
    return ret;
}

static inline void sys_exit(int64_t status)                       { sys3(SYS_EXIT, (uint64_t)status, 0, 0); for (;;) {} }
static inline int64_t sys_log(const void *p, size_t n)            { return sys3(SYS_LOG, (uint64_t)p, n, 0); }
static inline int64_t sys_send(uint64_t h, uint32_t type, const void *payload) {
    return sys3(SYS_SEND, h, type, (uint64_t)payload);
}
static inline int64_t sys_recv(uint64_t h, struct msg *out)       { return sys3(SYS_RECV, h, (uint64_t)out, 0); }
static inline int64_t sys_yield(void)                              { return sys3(SYS_YIELD, 0, 0, 0); }

static inline void put_str(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    sys_log(s, n);
}

#endif
