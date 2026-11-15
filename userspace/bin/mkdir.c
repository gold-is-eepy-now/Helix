/* mkdir.c: /bin/mkdir <dir>... — creates directories. */
#include <stdio.h>
#include "libhelix.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: mkdir <dir>...\n");
        return 1;
    }
    int status = 0;
    for (int i = 1; i < argc; i++) {
        if (helix_mkdir(argv[i]) != 0) {
            printf("mkdir: cannot create directory '%s'\n", argv[i]);
            status = 1;
        }
    }
    return status;
}
