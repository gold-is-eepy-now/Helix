/* boot_app.c — retain the optional .hlx boot module for the shell launcher. */
#include "boot_app.h"

enum { BOOT_APP_MAX = 8 };
typedef struct { const char *name; const void *data; size_t size; } boot_app_t;
static boot_app_t g_apps[BOOT_APP_MAX];
static size_t g_app_count;

static bool names_equal(const char *a, const char *b) {
    if (a == NULL || b == NULL) return false;
    while (*a != '\0' && *b != '\0' && *a == *b) { a++; b++; }
    return *a == *b;
}

bool boot_app_set(const char *name, const void *data, size_t size) {
    if (name == NULL || data == NULL || size == 0 || g_app_count == BOOT_APP_MAX)
        return false;
    for (size_t i = 0; i < g_app_count; i++)
        if (names_equal(g_apps[i].name, name)) return false;
    g_apps[g_app_count++] = (boot_app_t){ name, data, size };
    return true;
}

bool boot_app_get(const char *name, const char **resolved_name,
                  const void **data, size_t *size) {
    if (g_app_count == 0 || data == NULL || size == NULL) return false;
    for (size_t i = 0; i < g_app_count; i++) {
        if ((name == NULL || name[0] == '\0') || names_equal(g_apps[i].name, name)) {
            if (resolved_name != NULL) *resolved_name = g_apps[i].name;
            *data = g_apps[i].data;
            *size = g_apps[i].size;
            return true;
        }
    }
    return false;
}
