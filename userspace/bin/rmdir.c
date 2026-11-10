/* rmdir.c: /bin/rmdir <dir>... — removes empty directories. */
#include <stdio.h>
#include "libhelix.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: rmdir <dir>...\n");
        return 1;
    }
    int status = 0;
    for (int i = 1; i < argc; i++) {
        if (helix_rmdir(argv[i]) != 0) {
            printf("rmdir: failed to remove '%s'\n", argv[i]);
            status = 1;
        }
    }
    return status;
}
