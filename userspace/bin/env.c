/* env.c — /bin/env: prints the environment it was started with, one NAME=value per
 * line. It reads `environ`, which libhelix's crt sets from the envp block the
 * kernel built on the stack (usermode.c build_argv). */

#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    for (char **e = environ; *e != NULL; e++) {
        printf("%s\n", *e);
    }
    return 0;
}
