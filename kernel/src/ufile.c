/* ufile.c — per-process file descriptors on the ext4 root (see ufile.h).
 *
 * Path lookup starts at the root directory inode and walks one component at a
 * time with ext4_lookup. We do not use the boot filesystem (filesystem.c)
 * here: it holds in-memory boot files and its fs_write is still a stub. */
#include "ufile.h"

#include <stdbool.h>
#include <stddef.h>

#include "ext4.h"
#include "kstring.h"

typedef struct {
    bool used;
    uint64_t ino;
    uint64_t pos;
    bool readable;
    bool writable;
} ufd_t;

static ufd_t g_fds[UFILE_MAX_FDS];

void ufile_reset(void) {
    for (size_t i = 0; i < UFILE_MAX_FDS; i++) {
        g_fds[i].used = false;
    }
}

static bool fd_valid(int64_t fd) {
    return fd >= 3 && fd < (int64_t)UFILE_MAX_FDS && g_fds[fd].used;
}

/* Resolve an absolute path to an inode. Returns false if any component is
 * missing. `parent_out` receives the directory holding the final component and
 * `leaf` its name, so a caller can create the file when it is absent. */
static bool resolve(ext4_fs_t *fs, const char *path, ext4_stat_t *out,
                    uint64_t *parent_out, char *leaf, size_t leaf_cap) {
    if (path[0] != '/') return false;
    uint64_t cur = EXT4_ROOT_INO;
    ext4_stat_t st;
    st.ino = cur;
    st.mode = EXT4_S_IFDIR;
    st.size = 0;
    st.links = 0;
    st.mtime = 0;
    if (parent_out != NULL) *parent_out = cur;
    if (leaf != NULL && leaf_cap > 0) leaf[0] = '\0';

    const char *p = path;
    while (*p == '/') p++;
    while (*p != '\0') {
        char name[256];
        size_t n = 0;
        while (p[n] != '\0' && p[n] != '/') {
            if (n + 1 >= sizeof(name)) return false;
            name[n] = p[n];
            n++;
        }
        name[n] = '\0';
        p += n;
        while (*p == '/') p++;

        if (!EXT4_S_ISDIR(st.mode)) return false;
        if (parent_out != NULL) *parent_out = st.ino;
        if (leaf != NULL && leaf_cap > 0) {
            size_t k = 0;
            while (name[k] != '\0' && k + 1 < leaf_cap) { leaf[k] = name[k]; k++; }
            leaf[k] = '\0';
        }
        if (!ext4_lookup(fs, st.ino, name, &st)) return false;
    }
    if (out != NULL) *out = st;
    return true;
}

int64_t ufile_open(const char *path, uint64_t flags) {
    ext4_fs_t *fs = ext4_get_current();
    if (fs == NULL || path == NULL) return -1;

    int64_t fd = -1;
    for (int64_t i = 3; i < (int64_t)UFILE_MAX_FDS; i++) {
        if (!g_fds[i].used) { fd = i; break; }
    }
    if (fd < 0) return -1;

    ext4_stat_t st;
    uint64_t parent = 0;
    char leaf[256];
    if (!resolve(fs, path, &st, &parent, leaf, sizeof(leaf))) {
        /* Missing file: create it only when asked and the parent exists. */
        if (!(flags & UFILE_O_CREAT)) return -1;
        ext4_stat_t pst;
        char dir_leaf[256];
        size_t plen = strlen(path);
        /* Split off the last component, then resolve the directory above it. */
        char dir[256];
        if (plen >= sizeof(dir)) return -1;
        memcpy(dir, path, plen + 1);
        char *slash = strrchr(dir, '/');
        if (slash == NULL || slash[1] == '\0') return -1;
        if (slash == dir) dir[1] = '\0';
        else *slash = '\0';
        if (!resolve(fs, dir, &pst, &parent, dir_leaf, sizeof(dir_leaf))) return -1;
        if (!EXT4_S_ISDIR(pst.mode)) return -1;
        /* Name = the component after the last slash of the original path. */
        const char *name = strrchr(path, '/') + 1;
        if (!ext4_create(fs, pst.ino, name, EXT4_S_IFREG | 0644, &st.ino)) return -1;
        st.mode = EXT4_S_IFREG | 0644;
    } else {
        if (!EXT4_S_ISREG(st.mode)) return -1;   /* directories and links are not opened */
        if ((flags & UFILE_O_TRUNC) && (flags & UFILE_O_ACCMODE) != UFILE_O_RDONLY) {
            if (!ext4_truncate(fs, st.ino, 0)) return -1;
        }
    }

    uint64_t acc = flags & UFILE_O_ACCMODE;
    g_fds[fd].used = true;
    g_fds[fd].ino = st.ino;
    g_fds[fd].pos = 0;
    g_fds[fd].readable = acc == UFILE_O_RDONLY || acc == UFILE_O_RDWR;
    g_fds[fd].writable = acc == UFILE_O_WRONLY || acc == UFILE_O_RDWR;
    return fd;
}

int64_t ufile_read(int64_t fd, void *dst, uint64_t len) {
    ext4_fs_t *fs = ext4_get_current();
    if (fs == NULL || !fd_valid(fd) || !g_fds[fd].readable) return -1;
    uint64_t n = ext4_read(fs, g_fds[fd].ino, g_fds[fd].pos, dst, len);
    g_fds[fd].pos += n;
    return (int64_t)n;
}

int64_t ufile_write(int64_t fd, const void *src, uint64_t len) {
    ext4_fs_t *fs = ext4_get_current();
    if (fs == NULL || !fd_valid(fd) || !g_fds[fd].writable) return -1;
    uint64_t n = ext4_write(fs, g_fds[fd].ino, g_fds[fd].pos, src, len);
    g_fds[fd].pos += n;
    return (int64_t)n;
}

int64_t ufile_seek(int64_t fd, int64_t offset, int whence) {
    ext4_fs_t *fs = ext4_get_current();
    if (fs == NULL || !fd_valid(fd)) return -1;
    int64_t base = 0;
    if (whence == UFILE_SEEK_SET) {
        base = 0;
    } else if (whence == UFILE_SEEK_CUR) {
        base = (int64_t)g_fds[fd].pos;
    } else if (whence == UFILE_SEEK_END) {
        ext4_stat_t st;
        if (!ext4_stat(fs, g_fds[fd].ino, &st)) return -1;
        base = (int64_t)st.size;
    } else {
        return -1;
    }
    int64_t target = base + offset;
    if (target < 0) return -1;
    g_fds[fd].pos = (uint64_t)target;
    return target;
}

int64_t ufile_close(int64_t fd) {
    if (!fd_valid(fd)) return -1;
    g_fds[fd].used = false;
    return 0;
}
