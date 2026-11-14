/* fat32-test.c — host-side self-check for the in-kernel FAT32 driver.
 *
 * Built and run by tests/fat32-test.sh, which makes the images with mkfs.fat and mtools:
 *   fat32-test <srcdir> <imageA> <spcA> <imageB> <spcB> <fat16-image>
 * The driver is the real kernel/src/fat32.c, reading the images through a file-backed
 * block device. <srcdir> holds the host copies of the files planted in the images.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "block_device.h"
#include "fat32.h"

/* --- kernel stubs (the real fat32.c and block_device.c are linked) --- */
void *kmalloc(size_t n) { return malloc(n); }
void *kzalloc(size_t n) { return calloc(1, n); }
void kfree(void *p) { free(p); }

static int g_checks;
static int g_failures;

static void check(int cond, const char *what) {
    g_checks++;
    if (cond) {
        printf("[fat32-test] PASS: %s\n", what);
    } else {
        g_failures++;
        printf("[fat32-test] FAIL: %s\n", what);
    }
}

static void checkf(int cond, const char *fmt, const char *arg) {
    char buf[256];
    snprintf(buf, sizeof buf, fmt, arg);
    check(cond, buf);
}

/* --- file-backed block device --- */
static int g_fd = -1;

static uint64_t img_read(block_device_t *dev, uint64_t lba, void *buf, uint64_t blocks) {
    (void)dev;
    ssize_t n = pread(g_fd, buf, (size_t)(blocks * 512), (off_t)(lba * 512));
    return n < 0 ? 0 : (uint64_t)n / 512;
}

static block_device_t *open_device(const char *image) {
    g_fd = open(image, O_RDWR);
    if (g_fd < 0) { perror(image); exit(2); }
    struct stat st;
    fstat(g_fd, &st);
    block_device_t *dev = block_device_register("image", 512, (uint64_t)st.st_size / 512);
    dev->read = img_read;
    return dev;
}

static void close_device(block_device_t *dev) {
    block_device_unregister(dev);
    close(g_fd);
    g_fd = -1;
}

/* --- host files --- */
static uint8_t *host_file(const char *dir, const char *name, size_t *len) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n + 1);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { perror(path); exit(2); }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

static int host_has(const char *dir, const char *name) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    return access(path, F_OK) == 0;
}

/* --- directory listing --- */
#define MAX_LIST 128
typedef struct {
    char names[MAX_LIST][FAT32_NAME_BYTES];
    uint32_t sizes[MAX_LIST];
    int dirs[MAX_LIST];
    int count;
} listing_t;

static bool collect_cb(void *ctx, const fat32_entry_t *e) {
    listing_t *l = ctx;
    if (l->count < MAX_LIST) {
        strcpy(l->names[l->count], e->name);
        l->sizes[l->count] = e->size;
        l->dirs[l->count] = e->is_dir;
        l->count++;
    }
    return false;
}

static int list_index(const listing_t *l, const char *name) {
    for (int i = 0; i < l->count; i++) {
        if (strcmp(l->names[i], name) == 0) return i;
    }
    return -1;
}

/* Compare a whole file, and the size reported by stat, with the host copy. */
static void check_file(fat32_fs_t *fs, const char *fat_path, const char *src, const char *host_name) {
    size_t want_len = 0;
    uint8_t *want = host_file(src, host_name, &want_len);
    fat32_entry_t e;
    char msg[300];
    snprintf(msg, sizeof msg, "stat %s reports %zu bytes", fat_path, want_len);
    check(fat32_stat(fs, fat_path, &e) && !e.is_dir && e.size == want_len, msg);
    uint8_t *got = malloc(want_len + 64);
    /* Ask for more than the file holds: the driver must stop at the end of the file. */
    int64_t n = fat32_read(fs, fat_path, 0, got, want_len + 64);
    snprintf(msg, sizeof msg, "read %s returns every byte, unchanged", fat_path);
    check(n == (int64_t)want_len && memcmp(got, want, want_len) == 0, msg);
    free(got);
    free(want);
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Read a FAT entry straight from the image, so a test does not trust the driver. */
static uint32_t image_fat_entry(uint32_t cluster) {
    uint8_t bs[512];
    if (pread(g_fd, bs, sizeof bs, 0) != (ssize_t)sizeof bs) exit(2);
    uint32_t reserved = (uint32_t)(bs[14] | (bs[15] << 8));
    uint8_t e[4];
    if (pread(g_fd, e, 4, (off_t)reserved * 512 + (off_t)cluster * 4) != 4) exit(2);
    return le32(e) & 0x0FFFFFFF;
}

static void image_set_fat_entry(uint32_t cluster, uint32_t value) {
    uint8_t bs[512];
    if (pread(g_fd, bs, sizeof bs, 0) != (ssize_t)sizeof bs) exit(2);
    uint32_t reserved = (uint32_t)(bs[14] | (bs[15] << 8));
    uint8_t e[4] = { (uint8_t)value, (uint8_t)(value >> 8), (uint8_t)(value >> 16), (uint8_t)(value >> 24) };
    if (pwrite(g_fd, e, 4, (off_t)reserved * 512 + (off_t)cluster * 4) != 4) exit(2);
}


/* Make FRAG.BIN's cluster chain genuinely out of order: copy its second cluster to the last
 * free cluster on the volume and point the chain there. mtools writes files contiguously, so
 * the image is edited directly. Returns the number of jumps now in the chain (0 on failure). */
static uint32_t image_fragment(uint32_t first) {
    uint8_t bs[512];
    if (pread(g_fd, bs, sizeof bs, 0) != (ssize_t)sizeof bs) exit(2);
    uint32_t spc = bs[13], reserved = (uint32_t)(bs[14] | (bs[15] << 8)), nfats = bs[16];
    uint32_t fatsz = le32(bs + 36);
    uint32_t data_start = reserved + nfats * fatsz;
    uint32_t clusters = (le32(bs + 32) - data_start) / spc;
    uint32_t y = image_fat_entry(first);
    if (y < 2 || y >= 0x0FFFFFF7) return 0;
    uint32_t z = clusters + 1;
    while (z > 2 && image_fat_entry(z) != 0) z--;
    if (z <= y + 1) return 0;
    uint8_t *buf = malloc((size_t)spc * 512);
    off_t from = (off_t)(data_start + (y - 2) * spc) * 512, to = (off_t)(data_start + (z - 2) * spc) * 512;
    if (pread(g_fd, buf, (size_t)spc * 512, from) != (ssize_t)(spc * 512)) exit(2);
    if (pwrite(g_fd, buf, (size_t)spc * 512, to) != (ssize_t)(spc * 512)) exit(2);
    memset(buf, 0xEE, (size_t)spc * 512);            /* the old cluster must not be needed */
    if (pwrite(g_fd, buf, (size_t)spc * 512, from) != (ssize_t)(spc * 512)) exit(2);
    free(buf);
    uint32_t next_of_y = image_fat_entry(y);
    for (uint32_t f = 0; f < nfats; f++) {
        uint32_t base = (reserved + f * fatsz) * 512;
        uint32_t vals[4][2] = { { first, z }, { z, next_of_y }, { y, 0 }, { 0, 0 } };
        for (int k = 0; k < 3; k++) {
            uint32_t v = vals[k][1];
            uint8_t e[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
            if (pwrite(g_fd, e, 4, (off_t)base + (off_t)vals[k][0] * 4) != 4) exit(2);
        }
    }
    return 1;
}

static void test_volume(const char *name, const char *image, uint32_t want_spc, const char *src) {
    printf("[fat32-test] --- %s (%s) ---\n", name, image);
    block_device_t *dev = open_device(image);
    fat32_fs_t *fs = fat32_mount(dev, 0);
    check(fs != NULL, "the image mounts as FAT32");
    if (fs == NULL) {
        printf("[fat32-test] mount error: %s\n", fat32_mount_error());
        close_device(dev);
        return;
    }

    check(strcmp(fat32_label(fs), "HELIXFAT") == 0, "the volume label is read from the root directory");
    uint32_t bps = 0, spc = 0, clusters = 0, free_clusters = 0;
    fat32_info(fs, &bps, &spc, &clusters, &free_clusters);
    check(bps == 512, "bytes per sector is 512");
    check(spc == want_spc, "sectors per cluster matches mkfs.fat");
    check(clusters >= 65525, "the cluster count is in the FAT32 range");
    check(free_clusters > 0 && free_clusters < clusters, "the free cluster count is plausible");

    /* Root directory listing. */
    listing_t root;
    memset(&root, 0, sizeof root);
    check(fat32_list(fs, "/", collect_cb, &root), "the root directory lists");
    int i;
    i = list_index(&root, "HELLO.TXT");
    check(i >= 0 && !root.dirs[i] && root.sizes[i] == 18, "HELLO.TXT is a file of 18 bytes");
    i = list_index(&root, "readme-long-name.txt");
    check(i >= 0 && root.sizes[i] == 16, "the long name readme-long-name.txt is read in full");
    i = list_index(&root, "DOCS");
    check(i >= 0 && root.dirs[i] && root.sizes[i] == 0, "DOCS is a directory of size 0");
    i = list_index(&root, "BIG.BIN");
    check(i >= 0 && root.sizes[i] == 100000, "BIG.BIN is 100000 bytes");
    check(list_index(&root, "GONE.TXT") < 0, "a deleted file is not listed");
    check(list_index(&root, "HELIXFAT") < 0, "the volume label is not listed as a file");
    i = list_index(&root, "lower.txt");
    check(i >= 0, "an 8.3 name stored with the NT lowercase flags shows in lower case");

    if (host_has(src, "have-unicode")) {
        check(list_index(&root, "über-lång-filnamn-ünï.txt") >= 0,
              "a long name with accents is converted from UTF-16 to UTF-8");
        check(list_index(&root, "emoji-😀-file.txt") >= 0,
              "a long name with a surrogate pair becomes a four-byte UTF-8 character");
    } else {
        printf("[fat32-test] SKIP: unicode names (mtools could not create them here)\n");
    }

    /* Lookups: case-insensitive, nested, and with "." and "..". */
    fat32_entry_t e;
    check(fat32_stat(fs, "/hello.txt", &e) && e.size == 18, "lookup ignores case (hello.txt)");
    check(fat32_stat(fs, "/README-LONG-NAME.TXT", &e) && e.size == 16, "lookup ignores case for long names");
    check(fat32_stat(fs, "/docs/sub/deep.txt", &e) && e.size == 10, "a file two directories down is found");
    check(fat32_stat(fs, "/DOCS/SUB/../NESTED.TXT", &e) && e.size == 12, "\"..\" leaves a subdirectory");
    check(fat32_stat(fs, "/DOCS/./NESTED.TXT", &e) && e.size == 12, "\".\" is ignored");
    check(fat32_stat(fs, "/DOCS/SUB/../../HELLO.TXT", &e) && e.size == 18, "\"..\" twice returns to the root");
    check(fat32_stat(fs, "/..", &e) && e.is_dir, "\"..\" at the root stays at the root");
    check(fat32_stat(fs, "/", &e) && e.is_dir, "\"/\" is the root directory");
    check(!fat32_stat(fs, "/NOSUCH.TXT", &e), "a missing file is not found");
    check(!fat32_stat(fs, "/HELLO.TXT/x", &e), "a file cannot be used as a directory");
    check(!fat32_list(fs, "/HELLO.TXT", collect_cb, &root), "listing a file fails");

    /* Subdirectory listing. */
    listing_t docs;
    memset(&docs, 0, sizeof docs);
    check(fat32_list(fs, "/DOCS", collect_cb, &docs), "a subdirectory lists");
    check(list_index(&docs, ".") >= 0 && list_index(&docs, "..") >= 0, "\".\" and \"..\" entries are listed");
    check(list_index(&docs, "NESTED.TXT") >= 0 && list_index(&docs, "SUB") >= 0, "the subdirectory's entries are listed");

    /* Contents. */
    check_file(fs, "/HELLO.TXT", src, "hello.txt");
    check_file(fs, "/readme-long-name.txt", src, "readme-long-name.txt");
    check_file(fs, "/DOCS/NESTED.TXT", src, "nested.txt");
    check_file(fs, "/DOCS/SUB/DEEP.TXT", src, "deep.txt");
    check_file(fs, "/BIG.BIN", src, "big.bin");
    check_file(fs, "/FRAG.BIN", src, "frag.bin");
    if (host_has(src, "have-unicode")) check_file(fs, "/über-lång-filnamn-ünï.txt", src, "unicode.txt");

    /* Fragment FRAG.BIN by editing the image, then read it again through a fresh mount. */
    {
        fat32_entry_t fe;
        bool ok = fat32_stat(fs, "/FRAG.BIN", &fe);
        fat32_unmount(fs);
        check(ok && image_fragment(fe.first_cluster) == 1, "FRAG.BIN's chain is rearranged out of order");
        fs = fat32_mount(dev, 0);
        check(fs != NULL, "the volume mounts after the chain edit");
        if (fs == NULL) { close_device(dev); return; }
        check_file(fs, "/FRAG.BIN", src, "frag.bin");
    }

    /* Ranged reads over BIG.BIN: every offset and length, across cluster edges. */
    size_t big_len = 0;
    uint8_t *big = host_file(src, "big.bin", &big_len);
    static const uint64_t offsets[] = { 0, 1, 511, 512, 513, 4095, 4096, 4097, 8191, 8192, 12345, 99999, 100000, 100001 };
    static const uint64_t lengths[] = { 1, 3, 512, 777, 4096, 5000 };
    int bad = 0;
    uint8_t got[8192];
    for (size_t a = 0; a < sizeof offsets / sizeof offsets[0]; a++) {
        for (size_t b = 0; b < sizeof lengths / sizeof lengths[0]; b++) {
            uint64_t off = offsets[a], len = lengths[b];
            uint64_t want = off >= big_len ? 0 : (big_len - off < len ? big_len - off : len);
            int64_t n = fat32_read(fs, "/BIG.BIN", off, got, len);
            if (n != (int64_t)want || (want > 0 && memcmp(got, big + off, want) != 0)) bad++;
        }
    }
    check(bad == 0, "ranged reads at every offset and length match the host file");
    free(big);
    check(fat32_read(fs, "/DOCS", 0, got, 16) == -1, "reading a directory fails");
    check(fat32_read(fs, "/NOSUCH.BIN", 0, got, 16) == -1, "reading a missing file fails");

    fat32_unmount(fs);

    /* Corrupt the image: end BIG.BIN's chain after its first cluster. A full read must fail
     * cleanly instead of returning data, and a read inside the first cluster must still work. */
    fs = fat32_mount(dev, 0);
    if (fs != NULL && fat32_stat(fs, "/BIG.BIN", &e)) {
        uint32_t first = e.first_cluster;
        fat32_unmount(fs);
        image_set_fat_entry(first, 0x0FFFFFFF);
        fs = fat32_mount(dev, 0);
        check(fs != NULL, "the damaged image still mounts");
        if (fs != NULL) {
            check(fat32_read(fs, "/BIG.BIN", 0, got, sizeof got) == -1 ||
                  fat32_read(fs, "/BIG.BIN", 0, got, 100000 > sizeof got ? sizeof got : 100000) < 8192,
                  "a chain shorter than the file is detected");
            check(fat32_read(fs, "/BIG.BIN", 0, got, 100) == 100, "the first bytes of the damaged file still read");
            fat32_unmount(fs);
        }
    }
    close_device(dev);
}

int main(int argc, char **argv) {
    if (argc != 7) {
        fprintf(stderr, "usage: %s <srcdir> <imageA> <spcA> <imageB> <spcB> <fat16-image>\n", argv[0]);
        return 2;
    }
    const char *src = argv[1];
    test_volume("small clusters", argv[2], (uint32_t)atoi(argv[3]), src);
    test_volume("large clusters", argv[4], (uint32_t)atoi(argv[5]), src);

    /* Things that must be refused. */
    printf("[fat32-test] --- refused volumes ---\n");
    block_device_t *dev = open_device(argv[6]);
    fat32_fs_t *refused = fat32_mount(dev, 0);
    checkf(refused == NULL, "a FAT16 volume is refused (%s)", fat32_mount_error());
    close_device(dev);

    FILE *z = fopen("fat32-test-zero.img", "wb");
    static uint8_t zeros[512 * 64];
    fwrite(zeros, 1, sizeof zeros, z);
    fclose(z);
    dev = open_device("fat32-test-zero.img");
    refused = fat32_mount(dev, 0);
    checkf(refused == NULL, "a blank device is refused (%s)", fat32_mount_error());
    close_device(dev);
    remove("fat32-test-zero.img");

    printf("[fat32-test] RESULT: %s (%d checks, %d failures)\n", g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
