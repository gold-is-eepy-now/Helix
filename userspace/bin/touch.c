/* touch.c: /bin/touch <file>... — creates each file if it is missing; an existing
 * file is left as it is. */
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: touch <file>...\n");
        return 1;
    }
    int status = 0;
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_WRONLY | O_CREAT);
        if (fd < 0) {
            printf("touch: cannot touch '%s'\n", argv[i]);
            status = 1;
            continue;
        }
        close(fd);
    }
    return status;
}
