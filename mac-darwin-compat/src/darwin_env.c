/* darwin_env.c — Darwin process environment and dyld configuration.
 *
 * Part of Helix Mac/Darwin compatibility subsystem.
 */

#include "../include/darwin_env.h"
#include <string.h>

darwin_proc_env_t g_darwin_env;

#define MAX_ENV_ENTRIES 32
typedef struct {
    char key[64];
    char val[256];
    bool in_use;
} env_entry_t;

static env_entry_t g_env_vars[MAX_ENV_ENTRIES];

void darwin_env_init(void) {
    memset(&g_darwin_env, 0, sizeof(g_darwin_env));
    memset(g_env_vars, 0, sizeof(g_env_vars));

    g_darwin_env.pid = 501;
    g_darwin_env.ppid = 1;
    g_darwin_env.uid = 501;
    g_darwin_env.gid = 20;

    strncpy(g_darwin_env.user, "helix", sizeof(g_darwin_env.user) - 1);
    strncpy(g_darwin_env.home, "/Users/helix", sizeof(g_darwin_env.home) - 1);
    strncpy(g_darwin_env.shell, "/bin/zsh", sizeof(g_darwin_env.shell) - 1);
    strncpy(g_darwin_env.display, ":0.0", sizeof(g_darwin_env.display) - 1);
    strncpy(g_darwin_env.cwd, "/Users/helix", sizeof(g_darwin_env.cwd) - 1);
    strncpy(g_darwin_env.path_var, "/usr/bin:/bin:/usr/sbin:/sbin:/usr/local/bin", sizeof(g_darwin_env.path_var) - 1);
    strncpy(g_darwin_env.dyld_path, "/System/Library/Frameworks:/usr/lib", sizeof(g_darwin_env.dyld_path) - 1);

    darwin_setenv("USER", g_darwin_env.user, 1);
    darwin_setenv("LOGNAME", g_darwin_env.user, 1);
    darwin_setenv("HOME", g_darwin_env.home, 1);
    darwin_setenv("SHELL", g_darwin_env.shell, 1);
    darwin_setenv("PATH", g_darwin_env.path_var, 1);
    darwin_setenv("TMPDIR", "/private/tmp", 1);
    darwin_setenv("TERM", "xterm-256color", 1);
    darwin_setenv("DISPLAY", g_darwin_env.display, 1);
    darwin_setenv("DYLD_LIBRARY_PATH", g_darwin_env.dyld_path, 1);
}

const char *darwin_getenv(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < MAX_ENV_ENTRIES; i++) {
        if (g_env_vars[i].in_use && strcmp(g_env_vars[i].key, name) == 0) {
            return g_env_vars[i].val;
        }
    }
    return NULL;
}

int darwin_setenv(const char *name, const char *value, int overwrite) {
    if (!name || !value) return -1;

    for (int i = 0; i < MAX_ENV_ENTRIES; i++) {
        if (g_env_vars[i].in_use && strcmp(g_env_vars[i].key, name) == 0) {
            if (overwrite) {
                strncpy(g_env_vars[i].val, value, sizeof(g_env_vars[i].val) - 1);
                g_env_vars[i].val[sizeof(g_env_vars[i].val) - 1] = '\0';
            }
            return 0;
        }
    }

    for (int i = 0; i < MAX_ENV_ENTRIES; i++) {
        if (!g_env_vars[i].in_use) {
            strncpy(g_env_vars[i].key, name, sizeof(g_env_vars[i].key) - 1);
            g_env_vars[i].key[sizeof(g_env_vars[i].key) - 1] = '\0';
            strncpy(g_env_vars[i].val, value, sizeof(g_env_vars[i].val) - 1);
            g_env_vars[i].val[sizeof(g_env_vars[i].val) - 1] = '\0';
            g_env_vars[i].in_use = true;
            return 0;
        }
    }

    return -2;
}

const char *darwin_get_cwd(void) {
    return g_darwin_env.cwd;
}

int darwin_set_cwd(const char *path) {
    if (!path || path[0] != '/') return -1;
    strncpy(g_darwin_env.cwd, path, sizeof(g_darwin_env.cwd) - 1);
    g_darwin_env.cwd[sizeof(g_darwin_env.cwd) - 1] = '\0';
    return 0;
}
