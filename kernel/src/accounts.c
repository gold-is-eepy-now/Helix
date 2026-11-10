/* accounts.c — see accounts.h. */
#include "accounts.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "console.h"
#include "ext4.h"
#include "kstring.h"
#include "sha256.h"

#define ACCT_FILE       "etc/users"
#define ACCT_DIR        "etc"
#define ACCT_ITER       4096u
#define ACCT_SALT_LEN   16u
#define ACCT_FILE_MAX   (ACCT_MAX * 200u)
#define ACCT_ROOT_NAME  "root"

typedef struct {
    char name[ACCT_NAME_MAX + 1];
    uint32_t uid;
    uint32_t iterations;
    uint8_t salt[ACCT_SALT_LEN];
    uint8_t hash[SHA256_DIGEST_LEN];
} account_t;

static account_t g_acct[ACCT_MAX];
static uint32_t g_count;
static bool g_loaded;
static char g_session[ACCT_NAME_MAX + 1] = ACCT_ROOT_NAME;
static uint32_t g_session_uid;

/* ------------------------------------------------------------------ */
/* Randomness                                                          */
/* ------------------------------------------------------------------ */

static bool cpu_has_rdrand(void) {
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    return (c & (1u << 30)) != 0;
}

static bool rdrand64(uint64_t *out) {
    for (unsigned attempt = 0; attempt < 10; attempt++) {
        uint64_t v;
        unsigned char ok;
        __asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
        if (ok) {
            *out = v;
            return true;
        }
    }
    return false;
}

static uint64_t tsc_now(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

void accounts_random(uint8_t *buf, size_t n, bool *used_rdrand) {
    static bool probed;
    static bool have_rdrand;
    if (!probed) {
        probed = true;
        have_rdrand = cpu_has_rdrand();
    }
    if (used_rdrand) *used_rdrand = have_rdrand;

    size_t done = 0;
    while (done < n) {
        uint64_t v;
        if (have_rdrand && rdrand64(&v)) {
            size_t take = n - done < 8 ? n - done : 8;
            for (size_t i = 0; i < take; i++) buf[done + i] = (uint8_t)(v >> (8 * i));
            done += take;
        } else {
            /* Fallback: hash the timestamp counter. Predictable to anyone who
             * can time the boot, so it is only used when RDRAND is missing. */
            static uint64_t pool[4] = {0x9e3779b97f4a7c15ull, 0, 0, 0};
            uint64_t t = tsc_now();
            pool[0] ^= t;
            pool[1] += t << 13;
            uint8_t digest[SHA256_DIGEST_LEN];
            sha256(pool, sizeof pool, digest);
            memcpy(pool, digest, sizeof pool);
            size_t take = n - done < SHA256_DIGEST_LEN ? n - done : SHA256_DIGEST_LEN;
            memcpy(buf + done, digest, take);
            done += take;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Text helpers                                                        */
/* ------------------------------------------------------------------ */

static void to_hex(const uint8_t *in, size_t n, char *out) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = digits[in[i] >> 4];
        out[2 * i + 1] = digits[in[i] & 15];
    }
    out[2 * n] = '\0';
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool from_hex(const char *s, size_t n, uint8_t *out) {
    if (n % 2u != 0) return false;
    for (size_t i = 0; i < n / 2u; i++) {
        int hi = hex_val(s[2 * i]);
        int lo = hex_val(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static uint32_t parse_u32(const char **p) {
    uint32_t v = 0;
    while (**p >= '0' && **p <= '9') {
        v = v * 10u + (uint32_t)(**p - '0');
        (*p)++;
    }
    return v;
}

static bool hash_password(const account_t *a, const char *password, uint8_t out[SHA256_DIGEST_LEN]) {
    pbkdf2_sha256((const uint8_t *)password, strlen(password), a->salt, sizeof a->salt,
                  a->iterations, out, SHA256_DIGEST_LEN);
    return true;
}

/* Compare without an early exit, so timing does not reveal how much matched. */
static bool equal_ct(const uint8_t *a, const uint8_t *b, size_t n) {
    uint8_t diff = 0;
    for (size_t i = 0; i < n; i++) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

/* ------------------------------------------------------------------ */
/* Storage                                                             */
/* ------------------------------------------------------------------ */

static uint64_t lookup_dir(ext4_fs_t *fs) {
    ext4_stat_t st;
    if (!ext4_lookup(fs, EXT4_ROOT_INO, ACCT_DIR, &st)) return 0;
    return EXT4_S_ISDIR(st.mode) ? st.ino : 0;
}

static bool parse_table(const char *text, size_t len) {
    g_count = 0;
    size_t pos = 0;
    while (pos < len) {
        size_t end = pos;
        while (end < len && text[end] != '\n') end++;
        const char *line = text + pos;
        size_t n = end - pos;
        pos = end + 1;
        if (n == 0) continue;

        /* name:uid:iterations:salt:hash */
        if (g_count >= ACCT_MAX) break;
        account_t *a = &g_acct[g_count];
        memset(a, 0, sizeof *a);

        size_t i = 0;
        size_t nl = 0;
        while (i < n && line[i] != ':' && nl < ACCT_NAME_MAX) a->name[nl++] = line[i++];
        a->name[nl] = '\0';
        if (i >= n || line[i] != ':' || !accounts_name_valid(a->name)) continue;
        i++;

        const char *p = line + i;
        const char *end_line = line + n;
        a->uid = parse_u32(&p);
        if (p >= end_line || *p != ':') continue;
        p++;
        a->iterations = parse_u32(&p);
        if (p >= end_line || *p != ':' || a->iterations == 0) continue;
        p++;

        const char *salt_start = p;
        while (p < end_line && *p != ':') p++;
        if (p >= end_line || !from_hex(salt_start, (size_t)(p - salt_start), a->salt)) continue;
        if ((size_t)(p - salt_start) != 2u * ACCT_SALT_LEN) continue;
        p++;

        size_t hash_len = (size_t)(end_line - p);
        if (hash_len != 2u * SHA256_DIGEST_LEN || !from_hex(p, hash_len, a->hash)) continue;

        g_count++;
    }
    return true;
}

static bool save_table(void) {
    ext4_fs_t *fs = ext4_get_current();
    if (!fs) return false;

    /* Build the text in one buffer, then rewrite the file in one go. */
    static char text[ACCT_FILE_MAX];
    size_t len = 0;
    for (uint32_t i = 0; i < g_count; i++) {
        const account_t *a = &g_acct[i];
        char salt_hex[2 * ACCT_SALT_LEN + 1];
        char hash_hex[2 * SHA256_DIGEST_LEN + 1];
        to_hex(a->salt, sizeof a->salt, salt_hex);
        to_hex(a->hash, sizeof a->hash, hash_hex);

        char uid_text[12];
        char iter_text[12];
        uint32_t v = a->uid;
        size_t k = 0;
        char rev[12];
        do { rev[k++] = (char)('0' + v % 10); v /= 10; } while (v);
        for (size_t j = 0; j < k; j++) uid_text[j] = rev[k - 1 - j];
        uid_text[k] = '\0';
        v = a->iterations;
        k = 0;
        do { rev[k++] = (char)('0' + v % 10); v /= 10; } while (v);
        for (size_t j = 0; j < k; j++) iter_text[j] = rev[k - 1 - j];
        iter_text[k] = '\0';

        const char *parts[] = { a->name, ":", uid_text, ":", iter_text, ":", salt_hex, ":", hash_hex, "\n" };
        for (size_t j = 0; j < sizeof parts / sizeof parts[0]; j++) {
            size_t n = strlen(parts[j]);
            if (len + n >= sizeof text) return false;
            memcpy(text + len, parts[j], n);
            len += n;
        }
    }

    uint64_t dir = lookup_dir(fs);
    if (!dir) return false;
    ext4_stat_t st;
    uint64_t ino = 0;
    if (ext4_lookup(fs, dir, "users", &st)) {
        ino = st.ino;
        if (!ext4_truncate(fs, ino, 0)) return false;
    } else {
        if (!ext4_create(fs, dir, "users", EXT4_S_IFREG | 0600, &ino)) return false;
    }
    return ext4_write(fs, ino, 0, text, len) == len;
}

static bool sha256_self_test(void) {
    static const uint8_t want[SHA256_DIGEST_LEN] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
    };
    uint8_t got[SHA256_DIGEST_LEN];
    sha256("abc", 3, got);
    return memcmp(got, want, sizeof got) == 0;
}

bool accounts_init(void) {
    if (sha256_self_test()) {
        console_write("[crypto] sha256 self-test passed\n");
    } else {
        console_write("[crypto] sha256 self-test FAILED\n");
    }
    ext4_fs_t *fs = ext4_get_current();
    if (!fs) return false;

    if (!lookup_dir(fs)) {
        uint64_t made;
        if (!ext4_mkdir(fs, EXT4_ROOT_INO, ACCT_DIR, EXT4_S_IFDIR | 0755, &made)) return false;
    }

    g_loaded = true;
    g_count = 0;

    uint64_t dir = lookup_dir(fs);
    ext4_stat_t st;
    if (!dir || !ext4_lookup(fs, dir, "users", &st)) {
        console_write("[accounts] no account table yet\n");
        return true;
    }
    static char text[ACCT_FILE_MAX];
    uint64_t size = st.size < sizeof text ? st.size : sizeof text;
    uint64_t got = size ? ext4_read(fs, st.ino, 0, text, size) : 0;
    parse_table(text, (size_t)got);

    console_write("[accounts] ");
    console_write_dec(g_count);
    console_write(" account(s) loaded\n");
    return true;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

uint32_t accounts_count(void) {
    return g_count;
}

const char *accounts_name(uint32_t index) {
    return index < g_count ? g_acct[index].name : "";
}

bool accounts_name_valid(const char *name) {
    if (!name || name[0] == '\0' || strlen(name) > ACCT_NAME_MAX) return false;
    for (size_t i = 0; name[i]; i++) {
        char c = name[i];
        bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        bool digit = c >= '0' && c <= '9';
        if (i == 0 && !letter) return false;
        if (!letter && !digit && c != '-') return false;
    }
    return true;
}

static int find(const char *name) {
    for (uint32_t i = 0; i < g_count; i++) {
        if (strcmp(g_acct[i].name, name) == 0) return (int)i;
    }
    return -1;
}

static bool password_ok(const char *password) {
    return password && password[0] != '\0' && strlen(password) <= ACCT_PASS_MAX;
}

bool accounts_create(const char *name, const char *password, uint32_t *uid_out) {
    if (!g_loaded || !accounts_name_valid(name) || !password_ok(password)) return false;
    if (strcmp(name, ACCT_ROOT_NAME) == 0 || find(name) >= 0) return false;
    if (g_count >= ACCT_MAX) return false;

    account_t *a = &g_acct[g_count];
    memset(a, 0, sizeof *a);
    strcpy(a->name, name);
    a->uid = ACCT_FIRST_UID + g_count;
    a->iterations = ACCT_ITER;
    accounts_random(a->salt, sizeof a->salt, NULL);
    hash_password(a, password, a->hash);
    g_count++;

    if (!save_table()) {
        g_count--;
        return false;
    }
    if (uid_out) *uid_out = a->uid;
    return true;
}

bool accounts_verify(const char *name, const char *password, uint32_t *uid_out) {
    if (!g_loaded || !name || !password) return false;
    if (strcmp(name, ACCT_ROOT_NAME) == 0) return false;   /* root is the console, not a password login */
    int i = find(name);
    if (i < 0) {
        /* Hash anyway so an unknown name takes as long as a wrong password. */
        uint8_t dummy[SHA256_DIGEST_LEN];
        account_t fake;
        memset(&fake, 0, sizeof fake);
        fake.iterations = ACCT_ITER;
        hash_password(&fake, password, dummy);
        return false;
    }
    uint8_t got[SHA256_DIGEST_LEN];
    hash_password(&g_acct[i], password, got);
    if (!equal_ct(got, g_acct[i].hash, sizeof got)) return false;
    if (uid_out) *uid_out = g_acct[i].uid;
    return true;
}

bool accounts_set_password(const char *name, const char *password) {
    if (!g_loaded || !password_ok(password)) return false;
    int i = find(name);
    if (i < 0) return false;
    account_t *a = &g_acct[i];
    accounts_random(a->salt, sizeof a->salt, NULL);
    a->iterations = ACCT_ITER;
    hash_password(a, password, a->hash);
    return save_table();
}

const char *accounts_session(void) {
    return g_session;
}

uint32_t accounts_session_uid(void) {
    return g_session_uid;
}

void accounts_set_session(const char *name, uint32_t uid) {
    strncpy(g_session, name, sizeof g_session - 1);
    g_session[sizeof g_session - 1] = '\0';
    g_session_uid = uid;
}
