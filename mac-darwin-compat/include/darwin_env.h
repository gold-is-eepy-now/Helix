/* darwin_env.h — Darwin process environment and dyld configuration.
 *
 * Part of Helix Mac/Darwin compatibility subsystem.
 */

#ifndef MAC_DARWIN_ENV_H
#define MAC_DARWIN_ENV_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t pid;
    uint32_t ppid;
    uint32_t uid;
    uint32_t gid;
    char     cwd[1024];
    char     user[64];
    char     home[256];
    char     shell[128];
    char     display[64];
    char     dyld_path[512];
    char     path_var[512];
} darwin_proc_env_t;

extern darwin_proc_env_t g_darwin_env;

void        darwin_env_init(void);
const char *darwin_getenv(const char *name);
int         darwin_setenv(const char *name, const char *value, int overwrite);
const char *darwin_get_cwd(void);
int         darwin_set_cwd(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* MAC_DARWIN_ENV_H */
