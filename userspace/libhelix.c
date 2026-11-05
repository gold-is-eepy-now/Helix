/* libhelix.c — native Helix user-space API implementation.
 *
 * Wraps the kernel's native system-call interface (int $0x80, 7-register
 * x86-64 ABI). All functions are inline or direct syscall invocations
 * with no dependence on host libc.
 *
 * See kernel/src/syscall.h and runtime/linux_syscall.c for the ABI details.
 */

#include "libhelix.h"
#include <stdarg.h>

/* Program entry point — defined by each user program. */
extern int main(int argc, char **argv);

/* -- Program entry (crt0).
 * The kernel starts a program with rsp pointing at argc, then argv[0..argc-1], a
 * NULL, then the strings (usermode.c build_argv). _start passes that block to
 * helix_crt_main, which calls main and exits with its return value. */
__asm__(
    ".text\n"
    ".global _start\n"
    "_start:\n"
    "    mov %rsp, %rdi\n"
    "    and $-16, %rsp\n"
    "    call helix_crt_main\n"
    "    ud2\n");

__attribute__((noreturn))
void helix_crt_main(uint64_t *sp)
{
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    int status = main(argc, argv);
    helix_exit(status);
}

#include <string.h>

/* -- Files ----------------------------------------------------------------- */

int helix_open(const char *path, int flags)
{
    uint64_t r = helix_syscall(SYS_OPEN, (uint64_t)(uintptr_t)path, (uint64_t)flags, 0, 0, 0, 0, 0);
    return r == (uint64_t)-1 ? -1 : (int)r;
}

int64_t helix_read(int fd, void *buf, size_t count)
{
    uint64_t r = helix_syscall(SYS_READ, (uint64_t)fd, (uint64_t)(uintptr_t)buf, (uint64_t)count, 0, 0, 0, 0);
    return r == (uint64_t)-1 ? -1 : (int64_t)r;
}

int64_t helix_write(int fd, const void *buf, size_t count)
{
    uint64_t r = helix_syscall(SYS_WRITE, (uint64_t)fd, (uint64_t)(uintptr_t)buf, (uint64_t)count, 0, 0, 0, 0);
    return r == (uint64_t)-1 ? -1 : (int64_t)r;
}

int helix_close(int fd)
{
    uint64_t r = helix_syscall(SYS_CLOSE, (uint64_t)fd, 0, 0, 0, 0, 0, 0);
    return r == (uint64_t)-1 ? -1 : 0;
}

int64_t helix_lseek(int fd, int64_t offset, int whence)
{
    uint64_t r = helix_syscall(SYS_LSEEK, (uint64_t)fd, (uint64_t)offset, (uint64_t)whence, 0, 0, 0, 0);
    return r == (uint64_t)-1 ? -1 : (int64_t)r;
}

/* -- Memory management ----------------------------------------------------- */

void *helix_alloc(size_t bytes)
{
    uint64_t result = helix_syscall(SYS_ALLOC, bytes, 0, 0, 0, 0, 0, 0);
    if (result == (uint64_t)-1) return NULL;
    return (void *)result;
}

void helix_free(void *ptr)
{
    helix_syscall(SYS_FREE, (uint64_t)(uintptr_t)ptr, 0, 0, 0, 0, 0, 0);
}

int helix_map(void *virt, void *phys, uint64_t flags)
{
    uint64_t result = helix_syscall(SYS_MAP,
                                    (uint64_t)(uintptr_t)virt,
                                    (uint64_t)(uintptr_t)phys,
                                    flags, 0, 0, 0, 0);
    return (int)result;
}

int helix_unmap(void *virt)
{
    uint64_t result = helix_syscall(SYS_UNMAP,
                                    (uint64_t)(uintptr_t)virt, 0, 0, 0, 0, 0, 0);
    return (int)result;
}

uint64_t helix_brk(uint64_t brk)
{
    return helix_syscall(SYS_BRK, brk, 0, 0, 0, 0, 0, 0);
}

/* A small allocator on the program break. Every block has a 16-byte header
 * holding its usable size and, while free, a link to the next free block.
 * malloc takes the first free block that is large enough, else it moves the
 * break up. free puts the block on the free list. There is no coalescing, so
 * freed memory is reused only for requests it can hold. */
typedef struct hblock {
    size_t size;               /* usable bytes after the header */
    struct hblock *next;       /* free-list link (valid only while free) */
} hblock_t;

#define HBLOCK_HDR 16u
#define HALIGN     16u
static hblock_t *g_free_blocks;

void *helix_malloc(size_t bytes)
{
    size_t need = (bytes + HALIGN - 1u) & ~(size_t)(HALIGN - 1u);
    if (need == 0) need = HALIGN;

    /* First fit from the free list. */
    hblock_t **link = &g_free_blocks;
    while (*link != NULL) {
        hblock_t *b = *link;
        if (b->size >= need) {
            *link = b->next;
            return (char *)b + HBLOCK_HDR;
        }
        link = &b->next;
    }

    /* Grow the break. brk returns the old break if it refuses. */
    uint64_t cur = helix_brk(0);
    if (cur == 0) return NULL;
    uint64_t total = HBLOCK_HDR + need;
    uint64_t want = cur + total;
    if (helix_brk(want) != want) return NULL;

    hblock_t *b = (hblock_t *)(uintptr_t)cur;
    b->size = need;
    b->next = NULL;
    return (char *)b + HBLOCK_HDR;
}

size_t helix_msize(void *ptr)
{
    return ((hblock_t *)((char *)ptr - HBLOCK_HDR))->size;
}

int helix_console_getchar(void)
{
    for (;;) {
        uint64_t r = helix_syscall(SYS_READ_INPUT, 0, 0, 0, 0, 0, 0, 0);
        if ((int64_t)r >= 0) return (int)r;
        __asm__ volatile("pause");
    }
}

void helix_free_mem(void *ptr)
{
    if (ptr == NULL) return;
    hblock_t *b = (hblock_t *)((char *)ptr - HBLOCK_HDR);
    b->next = g_free_blocks;
    g_free_blocks = b;
}

/* -- Object / handle API --------------------------------------------------- */

handle_t helix_handle_create(void *object, uint32_t rights)
{
    uint64_t result = helix_syscall(SYS_HANDLE_CREATE,
                                    (uint64_t)(uintptr_t)object,
                                    (uint64_t)rights, 0, 0, 0, 0, 0);
    if (result == (uint64_t)-1) return (handle_t)-1;
    return (handle_t)result;
}

void *helix_handle_get(handle_t handle, uint32_t required_rights)
{
    uint64_t result = helix_syscall(SYS_HANDLE_GET,
                                    (uint64_t)handle,
                                    (uint64_t)required_rights, 0, 0, 0, 0, 0);
    if (result == (uint64_t)-1) return NULL;
    return (void *)result;
}

handle_t helix_handle_duplicate(handle_t source, uint32_t reduced_rights)
{
    uint64_t result = helix_syscall(SYS_HANDLE_DUPLICATE,
                                    (uint64_t)source,
                                    (uint64_t)reduced_rights, 0, 0, 0, 0, 0);
    if (result == (uint64_t)-1) return (handle_t)-1;
    return (handle_t)result;
}

bool helix_handle_close(handle_t handle)
{
    uint64_t result = helix_syscall(SYS_HANDLE_CLOSE, (uint64_t)handle, 0, 0, 0, 0, 0, 0);
    return result != 0;
}

void *helix_object_lookup(uint64_t object_id)
{
    uint64_t result = helix_syscall(SYS_OBJECT_LOOKUP, object_id, 0, 0, 0, 0, 0, 0);
    if (result == (uint64_t)-1) return NULL;
    return (void *)result;
}

bool helix_object_retain(void *object)
{
    uint64_t result = helix_syscall(SYS_OBJECT_RETAIN,
                                    (uint64_t)(uintptr_t)object, 0, 0, 0, 0, 0, 0);
    return result != 0;
}

void helix_object_release(void *object)
{
    helix_syscall(SYS_OBJECT_RELEASE, (uint64_t)(uintptr_t)object, 0, 0, 0, 0, 0, 0);
}

uint64_t helix_object_count(void)
{
    return helix_syscall(SYS_OBJECT_COUNT, 0, 0, 0, 0, 0, 0, 0);
}

/* -- Process control ------------------------------------------------------- */

int64_t helix_spawn(const char *path, char *const argv[])
{
    uint64_t r = helix_syscall(SYS_EXECVE, (uint64_t)(uintptr_t)path,
                               (uint64_t)(uintptr_t)argv, 0, 0, 0, 0, 0);
    return r == (uint64_t)-1 ? -1 : (int64_t)r;
}

void helix_exit(int status)
{
    helix_syscall(SYS_EXIT, (uint64_t)status, 0, 0, 0, 0, 0, 0);
    /* The kernel does not return here; if it ever did, spin rather than run on. */
    for (;;) {
        __asm__ volatile("pause");
    }
}

void helix_yield(void)
{
    helix_syscall(SYS_THREAD_YIELD, 0, 0, 0, 0, 0, 0, 0);
}

void *helix_thread_create(void (*entry)(void))
{
    uint64_t result = helix_syscall(SYS_THREAD_CREATE,
                                    (uint64_t)(uintptr_t)entry, 0, 0, 0, 0, 0, 0);
    if (result == (uint64_t)-1) return NULL;
    return (void *)result;
}

/* -- Channel IPC ----------------------------------------------------------- */

int helix_channel_send(handle_t channel, void *msg, size_t size)
{
    uint64_t result = helix_syscall(SYS_CHANNEL_SEND,
                                    (uint64_t)channel,
                                    (uint64_t)(uintptr_t)msg,
                                    (uint64_t)size, 0, 0, 0, 0);
    return (int)result;
}

long helix_channel_recv(handle_t channel, void *buf, size_t size)
{
    uint64_t result = helix_syscall(SYS_CHANNEL_RECV,
                                    (uint64_t)channel,
                                    (uint64_t)(uintptr_t)buf, 0, 0, 0, 0, 0);
    return (long)result;
}

/* -- String / utility ------------------------------------------------------ */

/* Write formatted output to fd through SYS_WRITE (fd 1 and 2 are the console).
 * Supports %s, %d, %u, %llu/%lu, %p only — no libc printf. */
int helix_write_fd(int fd, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);

    char buf[512];
    int len = 0;

    while (*fmt && len < (int)sizeof(buf) - 1) {
        if (*fmt != '%') {
            buf[len++] = *fmt++;
            continue;
        }
        fmt++;
        /* Consume "ll"/"l"/"h" length prefixes; only the final char matters. */
        while (*fmt == 'l' || *fmt == 'h') fmt++;
        switch (*fmt) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            while (*s && len < (int)sizeof(buf) - 1) buf[len++] = *s++;
            break;
        }
        case 'd': {
            int v = va_arg(ap, int);
            char tmp[16];
            int i = 0, neg = 0;
            if (v < 0) { neg = 1; v = -v; }
            do { tmp[i++] = (char)('0' + v % 10); v /= 10; } while (v);
            if (neg) buf[len++] = '-';
            while (i > 0 && len < (int)sizeof(buf) - 1) buf[len++] = tmp[--i];
            break;
        }
        case 'u': {
            /* %u / %lu / %llu: prefixes consumed above, LP64 = 64-bit. */
            unsigned long v = va_arg(ap, unsigned long);
            char tmp[24];
            int i = 0;
            do { tmp[i++] = (char)('0' + v % 10); v /= 10; } while (v);
            while (i > 0 && len < (int)sizeof(buf) - 1) buf[len++] = tmp[--i];
            break;
        }
        case 'p': {
            unsigned long v = (unsigned long)(uintptr_t)va_arg(ap, void *);
            char tmp[24];
            int i = 0;
            do { tmp[i++] = "0123456789abcdef"[v & 15]; v >>= 4; } while (v);
            if (len < (int)sizeof(buf) - 2) { buf[len++] = '0'; buf[len++] = 'x'; }
            while (i > 0 && len < (int)sizeof(buf) - 1) buf[len++] = tmp[--i];
            break;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            buf[len++] = c;
            break;
        }
        case '%':
            buf[len++] = '%';
            break;
        default:
            if (len < (int)sizeof(buf) - 2) { buf[len++] = '%'; buf[len++] = *fmt; }
            break;
        }
        if (*fmt) fmt++;
    }
    buf[len] = '\0';
    va_end(ap);

    /* One write call: the kernel moves it to the console (fd 1, 2) or to the
     * descriptor's file, in pieces of IO_CHUNK. */
    return (int)helix_write(fd, buf, (size_t)len);
}

/* Read a line from fd (1=stdin). */
char *helix_read_line(int fd)
{
    static char buf[256];
    char ch;
    int i;

    for (i = 0; i < (int)sizeof(buf) - 1; i++) {
        uint64_t result = helix_syscall(SYS_READ_INPUT, (uint64_t)fd, 0, 0, 0, 0, 0, 0);
        if (result == 0 || result == (uint64_t)-1) {
            if (i == 0) return NULL;
            break;
        }
        ch = (char)result;
        if (ch == '\n') break;
        buf[i] = ch;
    }
    buf[i] = '\0';
    return buf;
}

/* String duplication (program heap). */
char *helix_strdup(const char *s)
{
    size_t len = helix_strlen(s);
    char *dst = (char *)helix_malloc(len + 1);
    if (dst == NULL) return NULL;
    memcpy(dst, s, len + 1);
    return dst;
}

/* Length of a null-terminated string. */
size_t helix_strlen(const char *s)
{
    size_t n = 0;
    while (s[n] != '\0') n++;
    return n;
}

/* Compare two strings. */
int helix_strcmp(const char *s1, const char *s2)
{
    size_t i = 0;
    while (s1[i] != '\0' && s2[i] != '\0') {
        if (s1[i] != s2[i]) return (int)(unsigned char)s1[i] - (int)(unsigned char)s2[i];
        i++;
    }
    return (int)(unsigned char)s1[i] - (int)(unsigned char)s2[i];
}

/* Concatenate strings (program heap result). */
char *helix_strcat(char *restrict s1, const char *restrict s2)
{
    size_t n1 = helix_strlen(s1);
    size_t n2 = helix_strlen(s2);
    char *dst = (char *)helix_malloc(n1 + n2 + 1);
    if (dst == NULL) return NULL;
    memcpy(dst, s1, n1);
    memcpy(dst + n1, s2, n2);
    dst[n1 + n2] = '\0';
    return dst;
}

/* -- Linux compatibility shim ---------------------------------------------- */
int helix_linux_to_native(int linux_num)
{
    (void)linux_num;
    return -1;
}

int helix_errno(void)
{
    return 0;
}