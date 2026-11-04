/* compat_core.c — the Linux compatibility layer's kernel-side glue: allocation
 * wrappers and the initcall runner. Compiled with the kernel, not with drivers. */
#include <stddef.h>
#include <stdint.h>

#include "heap.h"
#include "linux_compat.h"
#include "panic.h"

typedef int (*compat_initcall_t)(void);

/* Bounds of the compat_initcalls section. GNU ld defines these for any section
 * whose name is a C identifier. */
extern compat_initcall_t __start_compat_initcalls[];
extern compat_initcall_t __stop_compat_initcalls[];

void *compat_kmalloc(size_t size) {
    return kmalloc(size);
}

void *compat_kzalloc(size_t size) {
    return kzalloc(size);
}

void compat_kfree(void *ptr) {
    kfree(ptr);
}

int compat_run_initcalls(void) {
    int failures = 0;
    size_t count = (size_t)(__stop_compat_initcalls - __start_compat_initcalls);
    for (size_t i = 0; i < count; i++) {
        int rc = __start_compat_initcalls[i]();
        if (rc != 0) {
            failures++;
            kprintf("[compat] initcall %u failed: %d\n", (unsigned)i, rc);
        }
    }
    kprintf("[compat] %u initcall(s) run, %d failed\n", (unsigned)count, failures);
    return failures;
}
