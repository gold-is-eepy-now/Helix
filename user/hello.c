/* hello.c — first C program using the tiny native Helix runtime. */
#include "helix.h"

void _start(void) {
    static const char greeting[] = "Hello from Helix user space!\n";
    helix_write(greeting, sizeof(greeting) - 1);
    helix_exit(42);
}
