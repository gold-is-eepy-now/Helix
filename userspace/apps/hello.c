/* hello.c — simple hello world program for Helix user space. */
#include <stdio.h>

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    printf("Hello from Helix user space!\n");
    return 0;
}
