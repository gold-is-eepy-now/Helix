/* filesystem.c — native Helix filesystem implementation.
 *
 * M11 milestone: storage and filesystems.
 *
 * Core filesystem abstraction:
 * - mount/unmount filesystem instances
 * - file open/read/write/close operations
 * - directory listing
 * - boot filesystem (read-only) loaded from a Limine module or memory
 *
 * Filesystems operate through objects. Each mount point is an object with
 * a Device.Control capability for user-space access.
 */

#include "filesystem.h"

#include "console.h"
#include "heap.h"
#include "kstring.h"
#include "object.h"

#define MAX_FILESYSTEMS 8

static filesystem_t *g_filesystems;
static uint8_t g_fs_count;

/* Boot filesystem: read-only, stores files from kernel image or ramdisk */
static file_entry_t *g_boot_root;
static uint8_t g_boot_file_count;

filesystem_t *fs_mount(const char *device_name, const char *mount_point,
                       const char *fs_name, bool read_only) {
    (void)device_name;
    (void)fs_name;

    if (g_fs_count >= MAX_FILESYSTEMS) return NULL;

    filesystem_t *fs = kzalloc(sizeof(*fs));
    if (fs == NULL) return NULL;

    fs->name = kzalloc(mount_point ? strlen(mount_point) : 1);
    if (fs->name == NULL) { kfree(fs); return NULL; }
    strcpy(fs->name, mount_point ? mount_point : "/");

    fs->mount_point = fs->name;

    /* Use boot filesystem root if already initialized, otherwise create new */
    if (g_boot_root != NULL) {
        fs->root = g_boot_root;
        g_boot_root->fs = fs;
    } else {
        fs->root = kzalloc(sizeof(*fs->root));
        if (fs->root == NULL) { kfree((void *)fs->name); kfree(fs); return NULL; }
        strcpy(fs->root->name, "/");
        fs->root->type = FS_DIR;
        fs->root->fs = fs;
    }

    fs->fs_object = object_create(OBJECT_TYPE_DEVICE, 0, NULL);
    if (fs->fs_object == NULL) { kfree(fs->root); kfree((void *)fs->name); kfree(fs); return NULL; }

    fs->read_only = read_only;

    /* Link into mounted list */
    fs->next = g_filesystems;
    g_filesystems = fs;
    g_fs_count++;

    console_write("[fs] mounted ");
    console_write(fs_name ? fs_name : "fs");
    console_write(" at ");
    console_write(mount_point ? mount_point : "/");
    console_putc('\n');

    return fs;
}

void fs_unmount(const char *mount_point) {
    if (mount_point == NULL) return;

    filesystem_t **link = &g_filesystems;
    while (*link != NULL) {
        if (strcmp((*link)->mount_point, mount_point) == 0) {
            filesystem_t *fs = *link;
            *link = fs->next;
            object_release(fs->fs_object);
            kfree(fs->root);
            kfree((void *)fs->name);
            kfree(fs);
            g_fs_count--;
            return;
        }
        link = &(*link)->next;
    }
}

filesystem_t *fs_find_mount(const char *path) {
    if (path == NULL) return NULL;

    filesystem_t *fs = g_filesystems;
    while (fs != NULL) {
        const char *mp = fs->mount_point;
        if (mp != NULL && strncmp(path, mp, strlen(mp)) == 0) {
            return fs;
        }
        fs = fs->next;
    }
    return NULL;
}

file_entry_t *fs_open(const char *path) {
    if (path == NULL || *path == '\0') return NULL;

    filesystem_t *fs = fs_find_mount(path);
    if (fs == NULL) return NULL;

    const char *name = path + strlen(fs->mount_point);
    if (*name == '/') name++;

    /* Walk through path components */
    file_entry_t *current = fs->root;
    if (strlen(name) == 0) return fs->root;

    while (*name != '\0') {
        /* Find next component */
        const char *slash = strchr(name, '/');
        size_t comp_len = slash ? (size_t)(slash - name) : strlen(name);

        if (comp_len == 0) {
            name = slash ? slash + 1 : name + strlen(name);
            continue;
        }

        /* Look for matching child */
        file_entry_t *child = current->children;
        bool found = false;
        while (child != NULL) {
            if (strncmp(child->name, name, comp_len) == 0 &&
                child->name[comp_len] == '\0') {
                current = child;
                found = true;
                break;
            }
            child = child->sibling;
        }

        if (!found) return NULL;

        name = slash ? slash + 1 : name + comp_len;
    }

    return current;
}

void fs_close(file_entry_t *file) {
    (void)file;
    /* In-memory filesystem: nothing to release */
}

uint64_t fs_read(file_entry_t *file, void *buffer, uint64_t count) {
    if (file == NULL || buffer == NULL || file->data == NULL) return 0;
    if (count > file->size - file->pos) count = file->size - file->pos;
    memcpy(buffer, file->data + file->pos, count);
    file->pos += count;
    return count;
}

uint64_t fs_write(file_entry_t *file, const void *buffer, uint64_t count) {
    (void)file; (void)buffer; (void)count;
    return 0; /* Not yet supported for boot filesystem */
}

void fs_register_mount_handler(const char *fs_name, fs_mount_fn mount) {
    (void)fs_name; (void)mount;
}

void fs_register_open_handler(const char *fs_name, fs_open_fn open) {
    (void)fs_name; (void)open;
}

void fs_register_read_handler(const char *fs_name, fs_read_fn read) {
    (void)fs_name; (void)read;
}

void fs_boot_init(void) {
    g_boot_root = kzalloc(sizeof(*g_boot_root));
    if (g_boot_root == NULL) return;
    strcpy(g_boot_root->name, "/");
    g_boot_root->type = FS_DIR;
    g_boot_root->fs = NULL;
    g_boot_file_count = 0;
}

void fs_boot_add_file(const char *name, const void *data, uint64_t size) {
    if (g_boot_file_count >= FS_MAX_FILES) return;

    file_entry_t *entry = kzalloc(sizeof(*entry));
    if (entry == NULL) return;

    strcpy(entry->name, name);
    entry->type = FS_FILE;
    entry->size = size;
    entry->data = (void *)data;
    entry->pos = 0;

    if (g_boot_root == NULL) {
        g_boot_root = entry;
    } else {
        entry->sibling = g_boot_root->children;
        g_boot_root->children = entry;
    }
    g_boot_file_count++;

    console_write("[fs] boot: added file ");
    console_write(name);
    console_write(" (");
    console_write_dec(size);
    console_write(" bytes)\n");
}

const char *fs_boot_root(void) {
    return g_boot_root ? "/" : NULL;
}

uint64_t fs_list_dir(file_entry_t *dir, file_entry_t **out_entries,
                     uint64_t max_entries) {
    if (dir == NULL || !(dir->type & FS_DIR)) return 0;

    uint64_t count = 0;
    file_entry_t *child = dir->children;
    while (child != NULL && count < max_entries) {
        out_entries[count++] = child;
        child = child->sibling;
    }
    return count;
}

uint64_t fs_file_size(file_entry_t *file) {
    return file ? file->size : 0;
}

bool fs_file_is_dir(file_entry_t *file) {
    return file ? (file->type & FS_DIR) != 0 : false;
}

void *file_get_data(file_entry_t *file) {
    return file ? file->data : NULL;
}