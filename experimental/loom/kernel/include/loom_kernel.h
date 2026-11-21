/* LOOM kernel-side declarations. Freestanding C11: no host libc. */
#ifndef LOOM_KERNEL_H
#define LOOM_KERNEL_H

#include <stdint.h>

void serial_init(void);
void serial_putc(char c);
void serial_puts(const char *s);
void serial_putu64(uint64_t v);
void serial_puthex64(uint64_t v);

__attribute__((noreturn)) void kpanic(const char *msg);

/* The LOOM scenario (kernel/src/demo.c). Returns true when it ends as the simulator's does. */
#include <stdbool.h>
bool loom_demo_run(void);

#endif
