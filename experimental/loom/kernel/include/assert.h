/* Kernel assert: a failed check prints where it failed over serial and halts. Freestanding
 * code has no libc, so the host's assert.h (which needs __assert_fail) is not used. */
#ifndef LOOM_KERNEL_ASSERT_H
#define LOOM_KERNEL_ASSERT_H

__attribute__((noreturn)) void kassert_fail(const char *expr, const char *file, int line);

#define assert(e) ((e) ? (void)0 : kassert_fail(#e, __FILE__, __LINE__))

#endif
