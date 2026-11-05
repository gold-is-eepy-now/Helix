/* userbin.c — installs the embedded ring-3 programs as files under /bin.
 *
 * The root is ext4 on the ramdisk, formatted at every boot, so /bin starts
 * empty each time. Writing the programs here, rather than baking a filesystem
 * image, means the programs live in the same place as everything else the
 * system creates: as files that exec() loads by path. */
#include "userbin.h"

#include <stddef.h>
#include <stdint.h>

#include "ext4.h"
#include "panic.h"
#include "userprogs.h"

#define INSTALL_CHUNK 4096u

bool userbin_install(void) {
    ext4_fs_t *fs = ext4_get_current();
    if (fs == NULL) return false;

    ext4_stat_t st;
    uint64_t bin = 0;
    if (ext4_lookup(fs, EXT4_ROOT_INO, "bin", &st)) {
        if (!EXT4_S_ISDIR(st.mode)) return false;
        bin = st.ino;
    } else if (!ext4_mkdir(fs, EXT4_ROOT_INO, "bin", EXT4_S_IFDIR | 0755, &bin)) {
        return false;
    }

    bool all = true;
    for (size_t i = 0; i < userprog_count(); i++) {
        const userprog_t *p = userprog_at(i);
        const char *name = p->install_path + 5;   /* skip "/bin/" */
        uint64_t size = (uint64_t)(p->end - p->start);

        uint64_t ino = 0;
        if (!ext4_create(fs, bin, name, EXT4_S_IFREG | 0755, &ino)) {
            all = false;
            continue;
        }
        uint64_t off = 0;
        while (off < size) {
            uint64_t n = size - off;
            if (n > INSTALL_CHUNK) n = INSTALL_CHUNK;
            if (ext4_write(fs, ino, off, p->start + off, n) != n) {
                all = false;
                break;
            }
            off += n;
        }
        if (off == size) {
            kprintf("[bin] installed %s (%llu bytes)\n", p->install_path,
                    (unsigned long long)size);
        }
    }
    return all;
}
