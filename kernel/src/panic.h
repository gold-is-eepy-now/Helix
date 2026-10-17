/* panic.h — formatted kernel output and fatal-error handling. */
#ifndef HELIX_PANIC_H
#define HELIX_PANIC_H

/* printf-style output to the console. Supports %s %c %d %u %x %p %% and the
 * l/ll length modifiers. */
void kprintf(const char *fmt, ...);

/* Print a message and halt the machine forever. Never returns. */
__attribute__((noreturn)) void panic(const char *fmt, ...);

/* Halt the CPU forever (no message). */
__attribute__((noreturn)) void halt_forever(void);

#endif /* HELIX_PANIC_H */
