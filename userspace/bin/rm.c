/* rm.c: /bin/rm <file>... — removes files (not directories). */
#include <stdio.h>
#include "libhelix.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: rm <file>...\n");
        return 1;
    }
    int status = 0;
    for (int i = 1; i < argc; i++) {
        if (helix_unlink(argv[i]) != 0) {
            printf("rm: cannot remove '%s'\n", argv[i]);
            status = 1;
        }
    }
    return status;
}
