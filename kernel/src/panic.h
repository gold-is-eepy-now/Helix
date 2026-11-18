/* panic.h — formatted kernel output and fatal-error handling. */
#ifndef HELIX_PANIC_H
#define HELIX_PANIC_H

#include <stdarg.h>

/* printf-style output to the console. Supports %s %c %d %u %x %p %% and the
 * l/ll length modifiers. */
void kprintf(const char *fmt, ...);

/* The same formatting, with each output character passed to fn (ctx is passed back).
 * Used by the kernel log to format into its ring instead of the console. */
typedef void (*kout_fn)(char c, void *ctx);
void kvprintf_to(kout_fn fn, void *ctx, const char *fmt, va_list args);

/* Print a message and halt the machine forever. Never returns. */
__attribute__((noreturn)) void panic(const char *fmt, ...);

/* Halt the CPU forever (no message). */
__attribute__((noreturn)) void halt_forever(void);

#endif /* HELIX_PANIC_H */
