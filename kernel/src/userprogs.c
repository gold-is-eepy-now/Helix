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
extern const uint8_t user_blob_echo_start[];
extern const uint8_t user_blob_echo_end[];
extern const uint8_t user_blob_true_start[];
extern const uint8_t user_blob_true_end[];
extern const uint8_t user_blob_false_start[];
extern const uint8_t user_blob_false_end[];
extern const uint8_t user_blob_cat_start[];
extern const uint8_t user_blob_cat_end[];
extern const uint8_t user_blob_ls_start[];
extern const uint8_t user_blob_ls_end[];
extern const uint8_t user_blob_mkdir_start[];
extern const uint8_t user_blob_mkdir_end[];
extern const uint8_t user_blob_rmdir_start[];
extern const uint8_t user_blob_rmdir_end[];
extern const uint8_t user_blob_rm_start[];
extern const uint8_t user_blob_rm_end[];
extern const uint8_t user_blob_touch_start[];
extern const uint8_t user_blob_touch_end[];
extern const uint8_t user_blob_cp_start[];
extern const uint8_t user_blob_cp_end[];
extern const uint8_t user_blob_find_start[];
extern const uint8_t user_blob_find_end[];
extern const uint8_t user_blob_edit_start[];
extern const uint8_t user_blob_edit_end[];
extern const uint8_t user_blob_ping_start[];
extern const uint8_t user_blob_ping_end[];

static const userprog_t g_progs[] = {
    { "sh",   "/bin/sh",   user_blob_sh_start,   user_blob_sh_end },
    { "init", "/bin/init", user_blob_init_start, user_blob_init_end },
    { "args", "/bin/args", user_blob_args_start, user_blob_args_end },
    { "env", "/bin/env", user_blob_env_start, user_blob_env_end },
    { "echo", "/bin/echo", user_blob_echo_start, user_blob_echo_end },
    { "true", "/bin/true", user_blob_true_start, user_blob_true_end },
    { "false", "/bin/false", user_blob_false_start, user_blob_false_end },
    { "cat", "/bin/cat", user_blob_cat_start, user_blob_cat_end },
    { "ls", "/bin/ls", user_blob_ls_start, user_blob_ls_end },
    { "mkdir", "/bin/mkdir", user_blob_mkdir_start, user_blob_mkdir_end },
    { "rmdir", "/bin/rmdir", user_blob_rmdir_start, user_blob_rmdir_end },
    { "rm", "/bin/rm", user_blob_rm_start, user_blob_rm_end },
    { "touch", "/bin/touch", user_blob_touch_start, user_blob_touch_end },
    { "cp", "/bin/cp", user_blob_cp_start, user_blob_cp_end },
    { "find", "/bin/find", user_blob_find_start, user_blob_find_end },
    { "edit", "/bin/edit", user_blob_edit_start, user_blob_edit_end },
    { "ping", "/bin/ping", user_blob_ping_start, user_blob_ping_end },
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
