/* cat.c: /bin/cat — copies each named file to the console. Files only; with no
 * arguments it prints its usage (there is no standard input yet). */
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: cat <file>...\n");
        return 1;
    }
    int status = 0;
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            printf("cat: %s: no such file\n", argv[i]);
            status = 1;
            continue;
        }
        char buf[512];
        for (;;) {
            ssize_t n = read(fd, buf, sizeof buf);
            if (n <= 0) break;
            write(1, buf, (size_t)n);
        }
        close(fd);
    }
    return status;
}
