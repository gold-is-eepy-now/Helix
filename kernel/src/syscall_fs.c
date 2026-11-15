/* syscall_fs.c — file-management system calls (stat, listdir, mkdir, unlink,
 * rmdir, truncate).
 *
 * Each handler copies its path from user memory with usermem_copy_str, then
 * calls the ufile layer, which resolves the path on the ext4 root. Kept in its
 * own file so syscall.c stays the dispatch table.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "heap.h"
#include "kstring.h"
#include "syscall.h"
#include "ufile.h"
#include "usermem.h"

/* The stat record ring-3 programs read. Its layout is mirrored in libhelix.h. */
typedef struct {
    uint64_t ino;
    uint64_t size;
    uint16_t mode;
    uint16_t links;
    uint32_t mtime;
} user_stat_t;

/* Longest listing a single call returns. Bigger directories are cut short. */
#define LISTDIR_MAX 8192u

uint64_t sys_stat(uint64_t path_addr, uint64_t out_addr, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    char path[256];
    if (!usermem_copy_str(path, sizeof(path), path_addr)) return UINT64_MAX;
    ufile_stat_t st;
    if (ufile_stat(path, &st) != 0) return UINT64_MAX;
    user_stat_t out = { st.ino, st.size, st.mode, st.links, st.mtime };
    if (!usermem_copy_out(out_addr, &out, sizeof(out))) return UINT64_MAX;
    return 0;
}

uint64_t sys_listdir(uint64_t path_addr, uint64_t buf_addr, uint64_t cap, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a4; (void)a5; (void)a6; (void)a7;
    char path[256];
    if (!usermem_copy_str(path, sizeof(path), path_addr)) return UINT64_MAX;
    if (cap == 0 || cap > LISTDIR_MAX) cap = LISTDIR_MAX;
    char *tmp = kmalloc(cap);
    if (tmp == NULL) return UINT64_MAX;
    int64_t n = ufile_listdir(path, tmp, cap);
    if (n < 0) { kfree(tmp); return UINT64_MAX; }
    bool ok = usermem_copy_out(buf_addr, tmp, (uint64_t)n);
    kfree(tmp);
    return ok ? (uint64_t)n : UINT64_MAX;
}

uint64_t sys_mkdir(uint64_t path_addr, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    char path[256];
    if (!usermem_copy_str(path, sizeof(path), path_addr)) return UINT64_MAX;
    return ufile_mkdir(path) == 0 ? 0 : UINT64_MAX;
}

uint64_t sys_unlink(uint64_t path_addr, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    char path[256];
    if (!usermem_copy_str(path, sizeof(path), path_addr)) return UINT64_MAX;
    return ufile_unlink(path) == 0 ? 0 : UINT64_MAX;
}

uint64_t sys_rmdir(uint64_t path_addr, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    char path[256];
    if (!usermem_copy_str(path, sizeof(path), path_addr)) return UINT64_MAX;
    return ufile_rmdir(path) == 0 ? 0 : UINT64_MAX;
}

uint64_t sys_truncate(uint64_t path_addr, uint64_t size, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    char path[256];
    if (!usermem_copy_str(path, sizeof(path), path_addr)) return UINT64_MAX;
    return ufile_truncate(path, size) == 0 ? 0 : UINT64_MAX;
}
