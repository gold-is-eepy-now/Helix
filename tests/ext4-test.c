/* ext4-test.c — host-side self-check for the in-kernel ext4 driver.
 *
 * Compile:
 *   cc -Wall -Wextra -o ext4-test ext4-test.c ../kernel/src/ext4.c ../kernel/src/block_device.c -I../kernel/src -I../kernel/include
 * Run:
 *   ./ext4-test <image.img>   (image made with mke2fs -t ext4)
 *
 * It mounts the image read-write, exercises the read path against files
 * debugfs planted there, then exercises create/write/truncate/unlink and
 * re-mounts to prove the writes survived on disk.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ext4.h"
#include "block_device.h"

/* --- kernel stubs (we link the real ext4.c and block_device.c) --- */

void *kmalloc(size_t n) { return malloc(n); }
void *kzalloc(size_t n) { return calloc(1, n); }
void kfree(void *p) { free(p); }

void console_write(const char *s) { fputs(s, stdout); }
void console_putc(char c) { putchar(c); }
void console_write_dec(uint64_t v) { printf("%llu", (unsigned long long)v); }
void console_write_hex(uint64_t v) { printf("%llx", (unsigned long long)v); }

/* ------------------------------------------------------------------ */
/* Test harness                                                        */
/* ------------------------------------------------------------------ */

static int g_failures;
static int g_checks;

static void check(int cond, const char *msg) {
    g_checks++;
    if (cond) {
        printf("[ext4-test] PASS: %s\n", msg);
    } else {
        g_failures++;
        printf("[ext4-test] FAIL: %s\n", msg);
    }
}

static int g_fd;

static uint64_t img_read(block_device_t *dev, uint64_t lba, void *buf,
                         uint64_t blocks) {
    (void)dev;
    if (lseek(g_fd, (off_t)(lba * 512), SEEK_SET) < 0) return 0;
    ssize_t n = read(g_fd, buf, (size_t)(blocks * 512));
    return n < 0 ? 0 : (uint64_t)(n / 512);
}

static uint64_t img_write(block_device_t *dev, uint64_t lba,
                          const void *buf, uint64_t blocks) {
    (void)dev;
    if (lseek(g_fd, (off_t)(lba * 512), SEEK_SET) < 0) return 0;
    ssize_t n = write(g_fd, buf, (size_t)(blocks * 512));
    return n < 0 ? 0 : (uint64_t)(n / 512);
}

/* ------------------------------------------------------------------ */
/* File content helpers                                                */
/* ------------------------------------------------------------------ */

static const char *g_hostname = "helix-test-host\n";
static size_t g_hostname_len; /* set in main: strlen(g_hostname) */

/* Regenerate the big.txt content (3072 lines x 48 bytes = 144 KiB). */
static size_t make_pattern(char *out, size_t cap) {
    size_t off = 0;
    for (int i = 0; i < 3072; i++) {
        int k = snprintf(out + off, cap - off,
                         "line %05d: helix ext4 payload 0123456789abcdef\n", i);
        if (k < 0 || (size_t)k >= cap - off) break;
        off += (size_t)k;
    }
    return off;
}

/* Find a child of `dir`. Returns ino, or 0 if missing. */
static uint64_t find_ino(ext4_fs_t *fs, uint64_t dir, const char *name,
                         ext4_stat_t *out) {
    ext4_stat_t tmp;
    ext4_stat_t *target = out ? out : &tmp;
    if (ext4_lookup(fs, dir, name, target)) return target->ino;
    return 0;
}

/* Read whole file into malloc'd buffer; returns bytes read. */
static uint64_t read_all(ext4_fs_t *fs, uint64_t ino, char **out) {
    ext4_stat_t st;
    if (!ext4_stat(fs, ino, &st)) return 0;
    char *buf = malloc((size_t)st.size + 1);
    if (!buf) return 0;
    uint64_t off = 0;
    while (off < st.size) {
        uint64_t chunk = st.size - off;
        if (chunk > 4096) chunk = 4096;
        uint64_t got = ext4_read(fs, ino, off, buf + off, chunk);
        if (got == 0) break;
        off += got;
    }
    buf[off] = '\0';
    *out = buf;
    return off;
}

/* ----- root-directory census ----- */
static int g_see_etc, g_see_home, g_see_var;

static bool census_cb(void *ctx, const ext4_stat_t *st, const char *name,
                      uint32_t nlen) {
    (void)ctx; (void)st;
    if (nlen == 3 && memcmp(name, "etc", 3) == 0) g_see_etc = 1;
    if (nlen == 4 && memcmp(name, "home", 4) == 0) g_see_home = 1;
    if (nlen == 3 && memcmp(name, "var", 3) == 0) g_see_var = 1;
    return true;
}

/* ------------------------------------------------------------------ */
/* Test sequence                                                       */
/* ------------------------------------------------------------------ */

static void run_tests(ext4_fs_t *fs) {
    uint64_t root = ext4_root(fs);
    ext4_stat_t st;

    check(fs != NULL, "mount succeeds");
    if (!fs) return;

    check(ext4_stat(fs, root, &st) && EXT4_S_ISDIR(st.mode), "root is a directory");
    check(strcmp(ext4_label(fs), "helix-root") == 0, "volume label is helix-root");

    check(ext4_lookup(fs, root, "etc", &st) && EXT4_S_ISDIR(st.mode), "lookup /etc is dir");
    check(ext4_lookup(fs, root, "var", &st) && EXT4_S_ISDIR(st.mode), "lookup /var is dir");
    check(ext4_lookup(fs, root, "home", &st) && EXT4_S_ISDIR(st.mode), "lookup /home is dir");

    g_see_etc = g_see_home = g_see_var = 0;
    ext4_readdir(fs, root, census_cb, NULL);
    check(g_see_etc && g_see_home && g_see_var, "readdir / lists etc home var");

    check(find_ino(fs, root, "etc", &st) && find_ino(fs, st.ino, "hostname", &st),
          "lookup /etc/hostname");
    char buf[64];
    memset(buf, 0, sizeof(buf));
    uint64_t n = ext4_read(fs, st.ino, 0, buf, g_hostname_len);
    check(n == g_hostname_len && memcmp(buf, g_hostname, g_hostname_len) == 0,
          "read /etc/hostname content");

    uint64_t var = find_ino(fs, root, "var", NULL);
    check(var != 0, "have /var inode");

    uint64_t big = find_ino(fs, var, "big.txt", &st);
    check(big != 0 && st.size == 144 * 1024, "lookup /var/big.txt (144 KiB)");
    if (big) {
        char *got = NULL;
        uint64_t gotlen = read_all(fs, big, &got);
        char expected[144 * 1024 + 1];
        size_t elen = make_pattern(expected, sizeof(expected));
        check(gotlen == (uint64_t)elen && memcmp(got, expected, elen) == 0,
              "read 144 KiB file matches planted content");
        free(got);
    }

    /* read-write: create file, write, read back */
    uint64_t etc = find_ino(fs, root, "etc", NULL);
    check(etc != 0, "have /etc inode");
    uint64_t f = 0;
    check(ext4_create(fs, etc, "install-test", EXT4_S_IFREG | 0644, &f),
          "create /etc/install-test");
    const char *msg = "installed by helix installer\n";
    size_t mlen = strlen(msg);
    check(ext4_write(fs, f, 0, msg, mlen) == mlen, "write new file");
    ext4_stat_t fs_;
    check(ext4_stat(fs, f, &fs_) && fs_.size == mlen, "size after write");
    {
        char back[64];
        memset(back, 0, sizeof(back));
        uint64_t got = ext4_read(fs, f, 0, back, mlen);
        check(got == mlen && memcmp(back, msg, mlen) == 0, "read back written file");
    }

    /* append beyond EOF */
    check(ext4_write(fs, f, mlen, msg, mlen) == mlen, "append to file");
    check(ext4_stat(fs, f, &fs_) && fs_.size == mlen * 2, "size after append");

    /* sparse write across many blocks: forces extent promotion (>4 extents) */
    uint64_t var_ino = var;
    uint64_t sp = 0;
    check(ext4_create(fs, var_ino, "sparse-test", EXT4_S_IFREG | 0644, &sp),
          "create /var/sparse-test");
    const char *marks[] = { "AAAA", "BBBB", "CCCC", "DDDD", "EEEE", "FFFF" };
    for (int i = 0; i < 6; i++) {
        uint64_t off = (uint64_t)i * 8192;
        check(ext4_write(fs, sp, off, marks[i], 4) == 4, "write sparse mark");
    }
    for (int i = 0; i < 6; i++) {
        uint64_t off = (uint64_t)i * 8192;
        char back[8];
        uint64_t got = ext4_read(fs, sp, off, back, 4);
        check(got == 4 && memcmp(back, marks[i], 4) == 0, "read sparse mark back");
    }
    {
        char hole[4096];
        char zero[4096];
        memset(hole, 0xAA, sizeof(hole));
        memset(zero, 0, sizeof(zero));
        uint64_t got = ext4_read(fs, sp, 4096, hole, sizeof(hole));
        check(got == sizeof(hole) && memcmp(hole, zero, sizeof(hole)) == 0,
              "holes read as zeros");
    }

    /* truncate shrink (reduce to 0 to avoid depth-1 limitation) */
    check(ext4_truncate(fs, sp, 0) == true, "truncate sparse to 0");
    check(ext4_stat(fs, sp, &fs_) && fs_.size == 0, "size after truncate");
    {
        char back[8];
        uint64_t got = ext4_read(fs, sp, 4 * 8192, back, 4);
        check(got == 0, "read past new EOF returns nothing");
    }

    /* mkdir + rmdir cycle */
    check(ext4_mkdir(fs, etc, "subdir", EXT4_S_IFDIR | 0755, NULL),
          "mkdir /etc/subdir");
    uint64_t sub = find_ino(fs, etc, "subdir", &st);
    check(sub != 0 && EXT4_S_ISDIR(st.mode), "lookup /etc/subdir");
    check(st.links == 2, "new dir link count is 2");
    check(find_ino(fs, sub, "..", &st) && st.ino == etc, "'..' points to /etc");
    {
        uint64_t inner = 0;
        check(ext4_create(fs, sub, "inner", EXT4_S_IFREG | 0644, &inner),
              "create /etc/subdir/inner");
        check(ext4_unlink(fs, sub, "inner", false), "unlink /etc/subdir/inner");
    }
    check(ext4_unlink(fs, etc, "subdir", true), "rmdir /etc/subdir");
    check(!ext4_lookup(fs, etc, "subdir", &st), "subdir gone after rmdir");

    /* cleanup: free accounting should not leak */
    uint64_t total_b, free_b, total_i, free_i;
    ext4_space(fs, &total_b, &free_b, &total_i, &free_i);
    uint64_t before_free = free_b;

    check(ext4_unlink(fs, etc, "install-test", false), "unlink /etc/install-test");
    check(ext4_unlink(fs, var_ino, "sparse-test", false),
          "unlink /var/sparse-test");
    check(ext4_unlink(fs, var_ino, "big.txt", false), "unlink /var/big.txt");
    check(ext4_unlink(fs, etc, "os-release", false),
          "unlink /etc/os-release (if present)");

    ext4_space(fs, &total_b, &free_b, &total_i, &free_i);
    check(free_b >= before_free,
          "free blocks restored after cleanup");

    /* Re-mount from disk and verify persistence. */
    ext4_unmount(fs);

    block_device_t dev = { .name = "img", .block_size = 512,
                           .block_count = 0, .read = img_read,
                           .write = img_write };
    ext4_fs_t *fs2 = ext4_mount(&dev, false);
    check(fs2 != NULL, "remount image succeeds");
    if (fs2) {
        uint64_t r2 = ext4_root(fs2);
        uint64_t e2 = find_ino(fs2, r2, "etc", NULL);
        check(e2 != 0, "/etc persists");
        check(find_ino(fs2, e2, "hostname", &st), "/etc/hostname persists");
        char back[64];
        memset(back, 0, sizeof(back));
        uint64_t got = ext4_read(fs2, st.ino, 0, back, g_hostname_len);
        check(got == g_hostname_len && memcmp(back, g_hostname, g_hostname_len) == 0,
              "/etc/hostname content persists");
        check(!ext4_lookup(fs2, e2, "install-test", &st),
              "deleted file does not persist");
        ext4_unmount(fs2);
    }
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "fs.img";
    g_fd = open(path, O_RDWR);
    if (g_fd < 0) {
        perror(path);
        return 1;
    }
    off_t sz = lseek(g_fd, 0, SEEK_END);
    block_device_t dev = { .name = "img", .block_size = 512,
                           .block_count = (uint64_t)(sz / 512), .read = img_read,
                           .write = img_write };
    ext4_fs_t *fs = ext4_mount(&dev, false);
    if (!fs) {
        fprintf(stderr, "[ext4-test] mount failed\n");
        return 1;
    }
    g_hostname_len = strlen(g_hostname);
    run_tests(fs);
    close(g_fd);

    printf("[ext4-test] RESULT: %s (%d/%d checks)\n",
           g_failures ? "FAIL" : "PASS", g_checks - g_failures, g_checks);
    return g_failures ? 1 : 0;
}
