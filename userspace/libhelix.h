#ifndef HELIX_LIBHELIX_H
#define HELIX_LIBHELIX_H

/* libhelix.h — native Helix user-space API.
 *
 * Provides a clean C API wrapping the kernel's native system-call interface
 * (int $0x80, 7-argument x86-64 ABI). This replaces inline assembly in
 * user programs with portable function calls.
 *
 * See kernel/src/syscall.h for the native syscall numbering convention.
 * See runtime/linux_syscall.c for Linux-to-native translation.
 *
 * Types re-exported from the kernel object model.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef uint32_t handle_t;

/* -----------------------------------------------------------
 * Low-level syscall wrapper (inline assembly).
 * ----------------------------------------------------------- */
static inline uint64_t helix_syscall(uint64_t native_num,
                                     uint64_t a1, uint64_t a2,
                                     uint64_t a3, uint64_t a4,
                                     uint64_t a5, uint64_t a6,
                                     uint64_t a7)
{
    uint64_t result;
    /* Arguments: rdi=a1, rsi=a2, rdx=a3, r10=a4, r8=a5, r9=a6.
     * Seventh argument (a7) goes on stack slot; kernel ignores it. */
    __asm__ volatile (
        "movq %1, %%rax\n"
        "movq %2, %%rdi\n"
        "movq %3, %%rsi\n"
        "movq %4, %%rdx\n"
        "movq %5, %%r10\n"
        "movq %6, %%r8\n"
        "movq %7, %%r9\n"
        "int $0x80\n"
        "movq %%rax, %0\n"
        : "=r"(result)
        : "r"(native_num), "r"(a1), "r"(a2), "r"(a3),
          "r"(a4), "r"(a5), "r"(a6), "r"(a7)
        : "rax", "rdi", "rsi", "rdx", "r10", "r8", "r9"
    );
    return result;
}

/* -----------------------------------------------------------
 * Native syscall numbers (match kernel/src/syscall.h) --
 * ----------------------------------------------------------- */
#define SYS_TEST               1
#define SYS_ALLOC              2
#define SYS_FREE               3
#define SYS_MAP                4
#define SYS_UNMAP              5
#define SYS_THREAD_CREATE      6
#define SYS_THREAD_EXIT        7
#define SYS_THREAD_YIELD       8
#define SYS_CHANNEL_SEND       9
#define SYS_CHANNEL_RECV       10
#define SYS_HANDLE_CREATE      11
#define SYS_HANDLE_CLOSE       12
#define SYS_HANDLE_DUPLICATE   13
#define SYS_HANDLE_GET         14
#define SYS_OBJECT_LOOKUP      15
#define SYS_OBJECT_RELEASE     16
#define SYS_OBJECT_COUNT       17
#define SYS_OBJECT_CREATE      18
#define SYS_OBJECT_RETAIN      19
#define SYS_READ               20
#define SYS_WRITE              21
#define SYS_DEBUG_WRITE        26   /* temporary console write; see helix_write_fd */
#define SYS_READ_INPUT         22
#define SYS_EXIT               23
#define SYS_BRK                24
#define SYS_EXECVE             25
#define SYS_OPEN               27   /* open(path, flags) */
#define SYS_CLOSE              28   /* close(fd) */
#define SYS_LSEEK              29   /* lseek(fd, offset, whence) */

/* Return values / error codes. */
#define HELIX_SUCCESS          0
#define HELIX_ERR_NONE         0
#define HELIX_ERR_NOMEM        1
#define HELIX_ERR_INVAL        2
#define HELIX_ERR_NOT_FOUND    3
#define HELIX_ERR_PERM         4
#define HELIX_ERR_EXIST        5
#define HELIX_ERR_IO           6
#define HELIX_ERR_NOENT        7
#define HELIX_ERR_FULL         8
#define HELIX_ERR_AGAIN        9
#define HELIX_ERR_INTR         10

/* -----------------------------------------------------------
 * Standard API function declarations.
 * ----------------------------------------------------------- */

/* Files. Paths are absolute ("/bin/sh"). Flags use the Linux values. */
#define HELIX_O_RDONLY 0x0
#define HELIX_O_WRONLY 0x1
#define HELIX_O_RDWR   0x2
#define HELIX_O_CREAT  0x40
#define HELIX_O_TRUNC  0x200
#define HELIX_SEEK_SET 0
#define HELIX_SEEK_CUR 1
#define HELIX_SEEK_END 2
int helix_open(const char *path, int flags);
int64_t helix_read(int fd, void *buf, size_t count);
int64_t helix_write(int fd, const void *buf, size_t count);
int helix_close(int fd);
int64_t helix_lseek(int fd, int64_t offset, int whence);

/* Memory management. helix_malloc/helix_free_mem use the program break (brk),
 * so they allocate from this process's own heap. helix_alloc is the older
 * kernel-heap call and is kept for the kernel test programs. */
void *helix_alloc(size_t bytes);
void helix_free(void *ptr);
void *helix_malloc(size_t bytes);
void helix_free_mem(void *ptr);
int helix_map(void *virt, void *phys, uint64_t flags);
int helix_unmap(void *virt);
uint64_t helix_brk(uint64_t brk);

/* Object / handle API. */
handle_t helix_handle_create(void *object, uint32_t rights);
void *helix_handle_get(handle_t handle, uint32_t required_rights);
handle_t helix_handle_duplicate(handle_t source, uint32_t reduced_rights);
bool helix_handle_close(handle_t handle);
void *helix_object_lookup(uint64_t object_id);
bool helix_object_retain(void *object);
void helix_object_release(void *object);
uint64_t helix_object_count(void);

/* Process control. */
__attribute__((noreturn)) void helix_exit(int status);

/* Run the program at the absolute path `path` as a child and wait for it. argv is
 * NULL-terminated; argv[0] is conventionally the path. Returns the child's exit
 * status, or -1 if it could not run. This kernel has no fork: the caller is not
 * replaced, it resumes with the child's status. */
int64_t helix_spawn(const char *path, char *const argv[]);

/* As helix_spawn, with an explicit environment (envp is NULL-terminated). Passing
 * NULL for envp gives the child this process's environment, as helix_spawn does. */
int64_t helix_spawn_env(const char *path, char *const argv[], char *const envp[]);
void helix_yield(void);
void *helix_thread_create(void (*entry)(void));

/* Channel IPC. */
int helix_channel_send(handle_t channel, void *msg, size_t size);
long helix_channel_recv(handle_t channel, void *buf, size_t size);

/* Size of a block returned by helix_malloc (its usable bytes). */
size_t helix_msize(void *p);

/* Wait for the next console key (blocking) and return it. */
int helix_console_getchar(void);

/* String / utility. */
int helix_write_fd(int fd, const char *fmt, ...);
char *helix_read_line(int fd);
char *helix_strdup(const char *s);
size_t helix_strlen(const char *s);
int helix_strcmp(const char *s1, const char *s2);
char *helix_strcat(char *restrict s1, const char *restrict s2);

/* -----------------------------------------------------------
 * Internal / low-level helpers (may change without notice).
 * ----------------------------------------------------------- */
int helix_linux_to_native(int linux_num);
int helix_errno(void);

/* --- End of libhelix.h --- */

#endif /* HELIX_LIBHELIX_H */