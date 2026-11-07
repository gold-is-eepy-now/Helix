/* userprogs.c — the table of embedded ring-3 programs (see userprogs.h).
 *
 * The symbols come from the generated user_blob.S (kernel/Makefile), which
 * .incbin's each built ELF between user_blob_<name>_start and _end. */
#include "userprogs.h"

extern const uint8_t user_blob_sh_start[];
extern const uint8_t user_blob_sh_end[];
extern const uint8_t user_blob_init_start[];
extern const uint8_t user_blob_init_end[];
extern const uint8_t user_blob_args_start[];
extern const uint8_t user_blob_args_end[];
extern const uint8_t user_blob_env_start[];
extern const uint8_t user_blob_env_end[];

static const userprog_t g_progs[] = {
    { "sh",   "/bin/sh",   user_blob_sh_start,   user_blob_sh_end },
    { "init", "/bin/init", user_blob_init_start, user_blob_init_end },
    { "args", "/bin/args", user_blob_args_start, user_blob_args_end },
    { "env",  "/bin/env",  user_blob_env_start,  user_blob_env_end },
};

#define PROG_COUNT (sizeof(g_progs) / sizeof(g_progs[0]))

static int same(const char *a, const char *b) {
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

const userprog_t *userprog_find(const char *name) {
    for (size_t i = 0; i < PROG_COUNT; i++) {
        if (same(g_progs[i].name, name)) return &g_progs[i];
    }
    return NULL;
}

size_t userprog_count(void) {
    return PROG_COUNT;
}

const userprog_t *userprog_at(size_t index) {
    return index < PROG_COUNT ? &g_progs[index] : NULL;
}
