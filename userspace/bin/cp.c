/* cp.c: /bin/cp <source> <dest> — copies one file. An existing destination is
 * replaced. */
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc != 3) {
        printf("usage: cp <source> <dest>\n");
        return 1;
    }
    int in = open(argv[1], O_RDONLY);
    if (in < 0) {
        printf("cp: cannot read '%s'\n", argv[1]);
        return 1;
    }
    int out = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0) {
        printf("cp: cannot write '%s'\n", argv[2]);
        close(in);
        return 1;
    }
    char buf[512];
    int status = 0;
    for (;;) {
        ssize_t n = read(in, buf, sizeof buf);
        if (n < 0) {
            printf("cp: read error on '%s'\n", argv[1]);
            status = 1;
            break;
        }
        if (n == 0) break;
        if (write(out, buf, (size_t)n) != n) {
            printf("cp: write error on '%s'\n", argv[2]);
            status = 1;
            break;
        }
    }
    close(in);
    close(out);
    return status;
}
