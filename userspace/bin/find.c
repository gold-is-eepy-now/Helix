/* find.c: /bin/find [dir] — prints every path below dir (default /), recursively.
 * Depth is capped so a deep tree cannot exhaust the program's stack. */
#include <stdio.h>
#include "libhelix.h"

#define LIST_CAP 8192
#define MAX_DEPTH 16

static int walk(const char *dir, int depth)
{
    if (depth > MAX_DEPTH) return 0;
    char *buf = (char *)helix_malloc(LIST_CAP);
    if (buf == NULL) return 1;
    int64_t n = helix_listdir(dir, buf, LIST_CAP);
    if (n < 0) {
        printf("find: cannot read %s\n", dir);
        helix_free_mem(buf);
        return 1;
    }
    const char *p = buf, *end = buf + n;
    while (p < end) {
        char kind = *p;
        const char *tab1 = p + 1;
        while (tab1 < end && *tab1 != '\t') tab1++;
        const char *tab2 = tab1 + 1;
        while (tab2 < end && *tab2 != '\t') tab2++;
        const char *name_at = tab2 + 1;
        const char *nl = name_at;
        while (nl < end && *nl != '\n') nl++;
        char path[512];
        int dir_len = 0;
        while (dir[dir_len] != '\0' && dir_len < 400) {
            path[dir_len] = dir[dir_len];
            dir_len++;
        }
        if (dir_len == 0 || path[dir_len - 1] != '/') path[dir_len++] = '/';
        for (const char *c = name_at; c < nl && dir_len < 500; c++) path[dir_len++] = *c;
        path[dir_len] = '\0';
        printf("%s\n", path);
        if (kind == 'd') walk(path, depth + 1);
        p = nl + 1;
    }
    helix_free_mem(buf);
    return 0;
}

int main(int argc, char **argv)
{
    const char *start = argc > 1 ? argv[1] : "/";
    return walk(start, 0);
}
