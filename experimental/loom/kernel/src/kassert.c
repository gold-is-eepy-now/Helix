#include "loom_kernel.h"

void kassert_fail(const char *expr, const char *file, int line) {
    serial_puts("LOOM ASSERT FAILED: ");
    serial_puts(expr);
    serial_puts(" at ");
    serial_puts(file);
    serial_puts(":");
    serial_putu64((uint64_t)line);
    serial_puts("\n");
    for (;;) __asm__ volatile ("cli; hlt");
}
