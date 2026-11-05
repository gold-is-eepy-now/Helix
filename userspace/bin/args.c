/* args.c — /bin/args: prints its arguments, one per line. It is a plain test of
 * argument passing and of the C runtime: the kernel builds argc and argv on the
 * stack (usermode.c build_argv), libhelix's _start hands them to main, and
 * printf formats the output through the libc in userspace/libc. */

#include <stdio.h>

int main(int argc, char **argv)
{
    printf("args: argc=%d\n", argc);
    for (int i = 0; i < argc; i++) {
        printf("args: argv[%d]=%s\n", i, argv[i]);
    }
    return 0;
}
