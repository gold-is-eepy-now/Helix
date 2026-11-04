/* mac_fs.c — Mac-like Filesystem (HFS+/APFS emulation).
 *
 * Implements case-insensitive, case-preserving path lookup, standard macOS
 * directory hierarchy, symlinks, extended attributes (xattrs), and open file descriptors.
 *
 * Part of Helix Mac/Darwin compatibility subsystem.
 */

#include "../include/mac_fs.h"
#include <string.h>

static mac_vnode_t g_nodes[MAC_MAX_NODES];
static uint32_t   g_node_count = 0;
static mac_vnode_t *g_root = NULL;
static mac_file_desc_t g_file_table[MAC_MAX_OPEN_FILES];
static bool g_fs_initialized = false;

/* Case-insensitive string comparison helper */
static int mac_strcasecmp(const char *s1, const char *s2) {
    while (*s1 && *s2) {
        char c1 = *s1;
        char c2 = *s2;
        if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
        if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
        if (c1 != c2) return (int)(unsigned char)c1 - (int)(unsigned char)c2;
        s1++;
        s2++;
    }
    char c1 = *s1;
    char c2 = *s2;
    if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
    if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
    return (int)(unsigned char)c1 - (int)(unsigned char)c2;
}

/* Allocate a new vnode */
static mac_vnode_t *alloc_node(const char *name, mac_node_type_t type, uint32_t mode) {
    if (g_node_count >= MAC_MAX_NODES) return NULL;

    mac_vnode_t *node = &g_nodes[g_node_count];
    memset(node, 0, sizeof(*node));
    node->ino = (uint64_t)(g_node_count + 1);
    g_node_count++;

    size_t l = 0;
    while (l < MAC_MAX_FILENAME && name[l]) {
        node->name[l] = name[l];
        l++;
    }
    node->name[l] = '\0';

    node->type = type;
    node->mode = mode;
    node->uid = 501; /* Standard macOS first user UID */
    node->gid = 20;  /* Standard macOS 'staff' GID */
    node->mtime = 1730764800; /* 2024+ timestamp */

    return node;
}

/* Add child to parent directory */
static void add_child(mac_vnode_t *parent, mac_vnode_t *child) {
    if (!parent || !child) return;
    child->parent = parent;
    child->next_sibling = parent->children;
    parent->children = child;
}

/* Case-insensitive child lookup in directory */
static mac_vnode_t *find_child(mac_vnode_t *parent, const char *name) {
    if (!parent || parent->type != MAC_NODE_DIR) return NULL;
    mac_vnode_t *curr = parent->children;
    while (curr) {
        if (mac_strcasecmp(curr->name, name) == 0) {
            return curr;
        }
        curr = curr->next_sibling;
    }
    return NULL;
}

/* Resolve path with case-insensitivity and optional symlink following */
mac_vnode_t *mac_fs_resolve_path(const char *path, bool follow_symlinks) {
    if (!g_fs_initialized) mac_fs_init();
    if (!path || path[0] == '\0') return NULL;

    mac_vnode_t *curr = g_root;
    if (path[0] != '/') return NULL; /* Absolute paths only for subsystem */

    char token[MAC_MAX_FILENAME + 1];
    const char *p = path;

    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;

        size_t tlen = 0;
        while (*p && *p != '/' && tlen < MAC_MAX_FILENAME) {
            token[tlen++] = *p++;
        }
        token[tlen] = '\0';

        if (strcmp(token, ".") == 0) {
            continue;
        } else if (strcmp(token, "..") == 0) {
            if (curr->parent) curr = curr->parent;
            continue;
        }

        mac_vnode_t *next = find_child(curr, token);
        if (!next) return NULL;

        if (next->type == MAC_NODE_SYMLINK && (follow_symlinks || *p != '\0')) {
            curr = mac_fs_resolve_path(next->target, true);
            if (!curr) return NULL;
        } else {
            curr = next;
        }
    }

    return curr;
}

/* Recursive directory creation */
mac_vnode_t *mac_fs_mkdir(const char *path, uint32_t mode) {
    if (!g_fs_initialized) mac_fs_init();
    if (!path || path[0] != '/') return NULL;

    mac_vnode_t *existing = mac_fs_resolve_path(path, true);
    if (existing) return (existing->type == MAC_NODE_DIR) ? existing : NULL;

    char parent_path[MAC_MAX_PATH];
    const char *last_slash = NULL;
    for (const char *s = path; *s; s++) {
        if (*s == '/') last_slash = s;
    }

    if (!last_slash || last_slash == path) {
        /* Direct child of root */
        const char *name = path + 1;
        mac_vnode_t *node = alloc_node(name, MAC_NODE_DIR, mode | 0755);
        if (node) add_child(g_root, node);
        return node;
    }

    size_t parent_len = (size_t)(last_slash - path);
    if (parent_len >= sizeof(parent_path)) parent_len = sizeof(parent_path) - 1;
    memcpy(parent_path, path, parent_len);
    parent_path[parent_len] = '\0';

    mac_vnode_t *parent_node = mac_fs_resolve_path(parent_path, true);
    if (!parent_node) {
        parent_node = mac_fs_mkdir(parent_path, mode);
        if (!parent_node) return NULL;
    }

    const char *name = last_slash + 1;
    mac_vnode_t *node = alloc_node(name, MAC_NODE_DIR, mode | 0755);
    if (node) add_child(parent_node, node);
    return node;
}

mac_vnode_t *mac_fs_create_file(const char *path, uint32_t mode) {
    if (!g_fs_initialized) mac_fs_init();
    if (!path || path[0] != '/') return NULL;

    mac_vnode_t *existing = mac_fs_resolve_path(path, false);
    if (existing) return (existing->type == MAC_NODE_FILE) ? existing : NULL;

    const char *last_slash = NULL;
    for (const char *s = path; *s; s++) {
        if (*s == '/') last_slash = s;
    }
    if (!last_slash) return NULL;

    char parent_path[MAC_MAX_PATH];
    size_t parent_len = (size_t)(last_slash - path);
    if (parent_len == 0) {
        strcpy(parent_path, "/");
    } else {
        if (parent_len >= sizeof(parent_path)) parent_len = sizeof(parent_path) - 1;
        memcpy(parent_path, path, parent_len);
        parent_path[parent_len] = '\0';
    }

    mac_vnode_t *parent = mac_fs_resolve_path(parent_path, true);
    if (!parent) {
        parent = mac_fs_mkdir(parent_path, 0755);
        if (!parent) return NULL;
    }

    const char *name = last_slash + 1;
    mac_vnode_t *node = alloc_node(name, MAC_NODE_FILE, mode | 0644);
    if (node) add_child(parent, node);
    return node;
}

mac_vnode_t *mac_fs_create_symlink(const char *path, const char *target) {
    if (!g_fs_initialized) mac_fs_init();
    if (!path || !target || path[0] != '/') return NULL;

    const char *last_slash = NULL;
    for (const char *s = path; *s; s++) {
        if (*s == '/') last_slash = s;
    }
    if (!last_slash) return NULL;

    char parent_path[MAC_MAX_PATH];
    size_t parent_len = (size_t)(last_slash - path);
    if (parent_len == 0) {
        strcpy(parent_path, "/");
    } else {
        if (parent_len >= sizeof(parent_path)) parent_len = sizeof(parent_path) - 1;
        memcpy(parent_path, path, parent_len);
        parent_path[parent_len] = '\0';
    }

    mac_vnode_t *parent = mac_fs_resolve_path(parent_path, true);
    if (!parent) {
        parent = mac_fs_mkdir(parent_path, 0755);
        if (!parent) return NULL;
    }

    const char *name = last_slash + 1;
    mac_vnode_t *node = alloc_node(name, MAC_NODE_SYMLINK, 0777);
    if (node) {
        strncpy(node->target, target, MAC_MAX_PATH - 1);
        node->target[MAC_MAX_PATH - 1] = '\0';
        add_child(parent, node);
    }
    return node;
}

int mac_fs_write_file_data(const char *path, const void *data, size_t len) {
    mac_vnode_t *node = mac_fs_resolve_path(path, true);
    if (!node) {
        node = mac_fs_create_file(path, 0644);
        if (!node) return -1;
    }
    if (node->type != MAC_NODE_FILE) return -2;

    /* Embedded node buffer backing up to 16 KiB */
    static uint8_t g_file_store[64][16384];
    static uint32_t g_store_idx = 0;

    if (!node->content) {
        if (g_store_idx < 64) {
            node->content = g_file_store[g_store_idx++];
            node->capacity = 16384;
        } else {
            return -3;
        }
    }

    if (len > node->capacity) len = node->capacity;
    memcpy(node->content, data, len);
    node->size = len;
    return (int)len;
}

int mac_fs_read_file_data(const char *path, void *buf, size_t maxlen, size_t *out_len) {
    mac_vnode_t *node = mac_fs_resolve_path(path, true);
    if (!node || node->type != MAC_NODE_FILE) return -1;

    size_t copy_len = node->size;
    if (copy_len > maxlen) copy_len = maxlen;
    if (node->content && copy_len > 0) {
        memcpy(buf, node->content, copy_len);
    }
    if (out_len) *out_len = copy_len;
    return (int)copy_len;
}

int mac_fs_stat(const char *path, mac_stat_t *st) {
    if (!st) return -1;
    mac_vnode_t *node = mac_fs_resolve_path(path, true);
    if (!node) return -2;

    memset(st, 0, sizeof(*st));
    st->st_ino = node->ino;
    st->st_size = node->size;
    st->st_mode = node->mode;
    st->st_uid = node->uid;
    st->st_gid = node->gid;
    st->st_mtime = node->mtime;
    st->st_nlink = 1;

    if (node->type == MAC_NODE_DIR) {
        st->st_mode |= 0040000; /* S_IFDIR */
    } else if (node->type == MAC_NODE_SYMLINK) {
        st->st_mode |= 0120000; /* S_IFLNK */
    } else {
        st->st_mode |= 0100000; /* S_IFREG */
    }

    return 0;
}

/* Initialize standard Mac filesystem directory layout */
void mac_fs_init(void) {
    if (g_fs_initialized) return;

    memset(g_nodes, 0, sizeof(g_nodes));
    memset(g_file_table, 0, sizeof(g_file_table));
    g_node_count = 0;

    /* Root directory / */
    g_root = alloc_node("", MAC_NODE_DIR, 0755);
    g_fs_initialized = true;

    /* Standard macOS directory hierarchy */
    mac_fs_mkdir("/System", 0755);
    mac_fs_mkdir("/System/Library", 0755);
    mac_fs_mkdir("/System/Library/Frameworks", 0755);
    mac_fs_mkdir("/System/Library/Frameworks/Foundation.framework", 0755);
    mac_fs_mkdir("/System/Library/Frameworks/AppKit.framework", 0755);
    mac_fs_mkdir("/System/Library/Frameworks/CoreGraphics.framework", 0755);
    mac_fs_mkdir("/System/Library/Frameworks/CoreFoundation.framework", 0755);
    mac_fs_mkdir("/System/Library/Frameworks/IOKit.framework", 0755);
    mac_fs_mkdir("/System/Library/CoreServices", 0755);

    mac_fs_mkdir("/Library", 0755);
    mac_fs_mkdir("/Library/Application Support", 0755);
    mac_fs_mkdir("/Library/Preferences", 0755);

    mac_fs_mkdir("/Applications", 0755);
    mac_fs_mkdir("/Users", 0755);
    mac_fs_mkdir("/Users/Shared", 0777);
    mac_fs_mkdir("/Users/helix", 0755);
    mac_fs_mkdir("/Users/helix/Desktop", 0755);
    mac_fs_mkdir("/Users/helix/Documents", 0755);
    mac_fs_mkdir("/Users/helix/Downloads", 0755);
    mac_fs_mkdir("/Users/helix/Library", 0755);
    mac_fs_mkdir("/Users/helix/Library/Preferences", 0755);
    mac_fs_mkdir("/Users/helix/Library/Application Support", 0755);

    mac_fs_mkdir("/Volumes", 0755);
    mac_fs_mkdir("/Volumes/Macintosh HD", 0755);

    mac_fs_mkdir("/private", 0755);
    mac_fs_mkdir("/private/var", 0755);
    mac_fs_mkdir("/private/etc", 0755);
    mac_fs_mkdir("/private/tmp", 01777);
    mac_fs_mkdir("/dev", 0755);
    mac_fs_mkdir("/bin", 0755);
    mac_fs_mkdir("/usr", 0755);
    mac_fs_mkdir("/usr/bin", 0755);
    mac_fs_mkdir("/usr/lib", 0755);
    mac_fs_mkdir("/usr/local", 0755);
    mac_fs_mkdir("/usr/local/bin", 0755);

    /* Canonical Darwin Symlinks */
    mac_fs_create_symlink("/etc", "/private/etc");
    mac_fs_create_symlink("/var", "/private/var");
    mac_fs_create_symlink("/tmp", "/private/tmp");

    /* Default SystemVersion.plist */
    const char *sys_ver_plist = 
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<plist version=\"1.0\">\n"
        "<dict>\n"
        "    <key>ProductBuildVersion</key>\n"
        "    <string>23F79</string>\n"
        "    <key>ProductName</key>\n"
        "    <string>macOS</string>\n"
        "    <key>ProductUserVisibleVersion</key>\n"
        "    <string>14.5</string>\n"
        "    <key>ProductVersion</key>\n"
        "    <string>14.5</string>\n"
        "</dict>\n"
        "</plist>\n";
    mac_fs_write_file_data("/System/Library/CoreServices/SystemVersion.plist",
                          sys_ver_plist, strlen(sys_ver_plist));
}

void mac_fs_teardown(void) {
    g_fs_initialized = false;
    g_node_count = 0;
    g_root = NULL;
}

/* File descriptor operations */
int mac_fs_open(const char *path, int flags, uint32_t mode) {
    (void)flags;
    mac_vnode_t *node = mac_fs_resolve_path(path, true);
    if (!node) {
        node = mac_fs_create_file(path, mode);
        if (!node) return -1;
    }

    for (int i = 0; i < MAC_MAX_OPEN_FILES; i++) {
        if (!g_file_table[i].in_use) {
            g_file_table[i].node = node;
            g_file_table[i].offset = 0;
            g_file_table[i].flags = flags;
            g_file_table[i].in_use = true;
            return i + 3; /* Allocate starting from 3 (0,1,2 = stdin, stdout, stderr) */
        }
    }
    return -2;
}

int64_t mac_fs_read(int fd, void *buf, size_t count) {
    int idx = fd - 3;
    if (idx < 0 || idx >= MAC_MAX_OPEN_FILES || !g_file_table[idx].in_use) {
        return -1;
    }

    mac_file_desc_t *desc = &g_file_table[idx];
    mac_vnode_t *node = desc->node;
    if (!node || node->type != MAC_NODE_FILE) return -2;

    if (desc->offset >= node->size) return 0; /* EOF */

    size_t available = node->size - (size_t)desc->offset;
    size_t to_read = count < available ? count : available;

    if (node->content && to_read > 0) {
        memcpy(buf, node->content + desc->offset, to_read);
        desc->offset += to_read;
    }
    return (int64_t)to_read;
}

int64_t mac_fs_write(int fd, const void *buf, size_t count) {
    int idx = fd - 3;
    if (idx < 0 || idx >= MAC_MAX_OPEN_FILES || !g_file_table[idx].in_use) {
        return -1;
    }

    mac_file_desc_t *desc = &g_file_table[idx];
    mac_vnode_t *node = desc->node;
    if (!node || node->type != MAC_NODE_FILE) return -2;

    if (mac_fs_write_file_data(node->name, buf, count) >= 0) {
        desc->offset += count;
        return (int64_t)count;
    }
    return -3;
}

int64_t mac_fs_lseek(int fd, int64_t offset, int whence) {
    int idx = fd - 3;
    if (idx < 0 || idx >= MAC_MAX_OPEN_FILES || !g_file_table[idx].in_use) {
        return -1;
    }

    mac_file_desc_t *desc = &g_file_table[idx];
    mac_vnode_t *node = desc->node;
    if (!node) return -2;

    int64_t new_off = desc->offset;
    if (whence == 0) { /* SEEK_SET */
        new_off = offset;
    } else if (whence == 1) { /* SEEK_CUR */
        new_off += offset;
    } else if (whence == 2) { /* SEEK_END */
        new_off = (int64_t)node->size + offset;
    } else {
        return -3;
    }

    if (new_off < 0) return -4;
    desc->offset = (uint64_t)new_off;
    return new_off;
}

int mac_fs_close(int fd) {
    int idx = fd - 3;
    if (idx < 0 || idx >= MAC_MAX_OPEN_FILES || !g_file_table[idx].in_use) {
        return -1;
    }
    g_file_table[idx].in_use = false;
    return 0;
}

int mac_fs_fstat(int fd, mac_stat_t *st) {
    int idx = fd - 3;
    if (idx < 0 || idx >= MAC_MAX_OPEN_FILES || !g_file_table[idx].in_use) {
        return -1;
    }
    mac_vnode_t *node = g_file_table[idx].node;
    if (!node) return -2;
    return mac_fs_stat(node->name, st);
}

int mac_fs_readdir(const char *path, mac_dirent_t *entries, size_t max_entries) {
    mac_vnode_t *dir = mac_fs_resolve_path(path, true);
    if (!dir || dir->type != MAC_NODE_DIR || !entries) return -1;

    size_t count = 0;
    mac_vnode_t *child = dir->children;
    while (child && count < max_entries) {
        entries[count].d_ino = child->ino;
        entries[count].d_type = (child->type == MAC_NODE_DIR) ? 4 :
                                (child->type == MAC_NODE_SYMLINK) ? 10 : 8;
        size_t nlen = strlen(child->name);
        if (nlen > MAC_MAX_FILENAME) nlen = MAC_MAX_FILENAME;
        entries[count].d_namlen = (uint8_t)nlen;
        memcpy(entries[count].d_name, child->name, nlen);
        entries[count].d_name[nlen] = '\0';
        count++;
        child = child->next_sibling;
    }
    return (int)count;
}

/* Extended Attributes (xattr) implementation */
int mac_fs_setxattr(const char *path, const char *name, const void *value, size_t size, int flags) {
    (void)flags;
    mac_vnode_t *node = mac_fs_resolve_path(path, true);
    if (!node || !name || !value) return -1;
    if (size > MAC_MAX_XATTR_VAL) return -2;

    /* Check if attribute already exists */
    for (uint32_t i = 0; i < node->xattr_count; i++) {
        if (strcmp(node->xattrs[i].key, name) == 0) {
            memcpy(node->xattrs[i].value, value, size);
            node->xattrs[i].size = size;
            return 0;
        }
    }

    if (node->xattr_count < MAC_MAX_XATTRS) {
        mac_xattr_t *xa = &node->xattrs[node->xattr_count++];
        strncpy(xa->key, name, MAC_MAX_XATTR_KEY - 1);
        xa->key[MAC_MAX_XATTR_KEY - 1] = '\0';
        memcpy(xa->value, value, size);
        xa->size = size;
        xa->in_use = true;
        return 0;
    }

    return -3;
}

int mac_fs_getxattr(const char *path, const char *name, void *value, size_t size) {
    mac_vnode_t *node = mac_fs_resolve_path(path, true);
    if (!node || !name) return -1;

    for (uint32_t i = 0; i < node->xattr_count; i++) {
        if (strcmp(node->xattrs[i].key, name) == 0) {
            if (!value || size == 0) {
                return (int)node->xattrs[i].size; /* Query size */
            }
            size_t copy_size = node->xattrs[i].size;
            if (copy_size > size) copy_size = size;
            memcpy(value, node->xattrs[i].value, copy_size);
            return (int)copy_size;
        }
    }

    return -2;
}

int mac_fs_listxattr(const char *path, char *namebuf, size_t size) {
    mac_vnode_t *node = mac_fs_resolve_path(path, true);
    if (!node) return -1;

    size_t total_needed = 0;
    for (uint32_t i = 0; i < node->xattr_count; i++) {
        total_needed += strlen(node->xattrs[i].key) + 1;
    }

    if (!namebuf || size == 0) {
        return (int)total_needed;
    }

    char *dst = namebuf;
    size_t remaining = size;
    for (uint32_t i = 0; i < node->xattr_count; i++) {
        size_t klen = strlen(node->xattrs[i].key) + 1;
        if (remaining >= klen) {
            memcpy(dst, node->xattrs[i].key, klen);
            dst += klen;
            remaining -= klen;
        }
    }

    return (int)(dst - namebuf);
}

int mac_fs_removexattr(const char *path, const char *name) {
    mac_vnode_t *node = mac_fs_resolve_path(path, true);
    if (!node || !name) return -1;

    for (uint32_t i = 0; i < node->xattr_count; i++) {
        if (strcmp(node->xattrs[i].key, name) == 0) {
            for (uint32_t j = i; j + 1 < node->xattr_count; j++) {
                node->xattrs[j] = node->xattrs[j + 1];
            }
            node->xattr_count--;
            return 0;
        }
    }
    return -2;
}
