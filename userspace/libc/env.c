/* env.c — the process environment: environ, getenv, setenv, unsetenv.
 *
 * The kernel builds the environment on the initial stack (usermode.c build_argv)
 * and the C runtime points `environ` at it (libhelix.c helix_crt_main). That block
 * is read-only in practice, so the first change copies the pointer array to the
 * heap and all later changes work on the copy.
 *
 * Limitation: a string that setenv replaces or unsetenv removes is not freed. The
 * environment is small and changes rarely, so this is kept simple. */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

char **environ;

/* 1 once `environ` is a heap array this library owns. */
static int env_owned;

static size_t env_count(void) {
    size_t n = 0;
    if (environ == NULL) return 0;
    while (environ[n] != NULL) n++;
    return n;
}

/* True if `entry` is "name=..." for the given name of length nlen. */
static int entry_is(const char *entry, const char *name, size_t nlen) {
    return strncmp(entry, name, nlen) == 0 && entry[nlen] == '=';
}

static long env_find(const char *name, size_t nlen) {
    size_t n = env_count();
    for (size_t i = 0; i < n; i++) {
        if (entry_is(environ[i], name, nlen)) return (long)i;
    }
    return -1;
}

/* Make `environ` a heap array with room for `extra` more entries. */
static int env_own(size_t extra) {
    size_t n = env_count();
    char **a = malloc((n + extra + 1) * sizeof(char *));
    if (a == NULL) return -1;
    for (size_t i = 0; i < n; i++) a[i] = environ[i];
    a[n] = NULL;
    if (env_owned) free(environ);   /* our previous array; the strings it points to stay */
    environ = a;
    env_owned = 1;
    return 0;
}

char *getenv(const char *name) {
    if (name == NULL || name[0] == '\0') return NULL;
    size_t nlen = strlen(name);
    long i = env_find(name, nlen);
    if (i < 0) return NULL;
    return environ[i] + nlen + 1;
}

int setenv(const char *name, const char *value, int overwrite) {
    if (name == NULL || name[0] == '\0' || strchr(name, '=') != NULL) {
        errno = EINVAL;
        return -1;
    }
    size_t nlen = strlen(name);
    long i = env_find(name, nlen);
    if (i >= 0 && !overwrite) return 0;

    size_t vlen = strlen(value);
    char *entry = malloc(nlen + 1 + vlen + 1);
    if (entry == NULL) return -1;
    memcpy(entry, name, nlen);
    entry[nlen] = '=';
    memcpy(entry + nlen + 1, value, vlen + 1);

    if (i >= 0) {
        if (!env_owned && env_own(0) != 0) return -1;
        environ[i] = entry;
        return 0;
    }
    if (env_own(1) != 0) return -1;
    size_t n = env_count();
    environ[n] = entry;
    environ[n + 1] = NULL;
    return 0;
}

int unsetenv(const char *name) {
    if (name == NULL || name[0] == '\0' || strchr(name, '=') != NULL) {
        errno = EINVAL;
        return -1;
    }
    size_t nlen = strlen(name);
    long i = env_find(name, nlen);
    if (i < 0) return 0;
    if (!env_owned && env_own(0) != 0) return -1;
    size_t n = env_count();
    for (size_t k = (size_t)i; k + 1 < n; k++) environ[k] = environ[k + 1];
    environ[n - 1] = NULL;
    return 0;
}
