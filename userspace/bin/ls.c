/* ls.c: /bin/ls [path] — lists a directory, one entry per line. */
#include <stdio.h>
#include "libhelix.h"

#define LIST_CAP 8192

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/";
    char *buf = (char *)helix_malloc(LIST_CAP);
    if (buf == NULL) {
        printf("ls: out of memory\n");
        return 1;
    }
    int64_t n = helix_listdir(path, buf, LIST_CAP);
    if (n < 0) {
        printf("ls: cannot read %s\n", path);
        helix_free_mem(buf);
        return 1;
    }
    /* Each line is "kind\tsize\tname\n". */
    const char *p = buf, *end = buf + n;
    while (p < end) {
        char kind = *p;
        const char *tab1 = p + 1;
        while (tab1 < end && *tab1 != '\t') tab1++;
        const char *size_at = tab1 + 1;
        const char *tab2 = size_at;
        while (tab2 < end && *tab2 != '\t') tab2++;
        const char *name_at = tab2 + 1;
        const char *nl = name_at;
        while (nl < end && *nl != '\n') nl++;
        unsigned long long size = 0;
        for (const char *d = size_at; d < tab2; d++) size = size * 10 + (unsigned)(*d - '0');
        char name[256];
        size_t len = 0;
        while (name_at + len < nl && len < sizeof name - 1) {
            name[len] = name_at[len];
            len++;
        }
        name[len] = '\0';
        if (kind == 'd') {
            printf("[DIR]  %s\n", name);
        } else {
            printf("%s (%llu bytes)\n", name, size);
        }
        p = nl + 1;
    }
    helix_free_mem(buf);
    return 0;
}
