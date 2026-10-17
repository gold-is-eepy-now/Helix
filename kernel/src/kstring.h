/* kstring.h — freestanding memory and string primitives.
 *
 * A freestanding kernel has no libc, but the compiler still emits calls to
 * memcpy/memset for struct copies and array initialisation, and the kernel
 * itself needs these everywhere. This is also the seed of a native runtime
 * that user space will reuse later.
 */
#ifndef HELIX_KSTRING_H
#define HELIX_KSTRING_H

#include <stddef.h>

void *memcpy(void *dest, const void *src, size_t n);
void *memmove(void *dest, const void *src, size_t n);
void *memset(void *dest, int value, size_t n);
int memcmp(const void *a, const void *b, size_t n);

size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strcpy(char *dest, const char *src);
char *strncpy(char *dest, const char *src, size_t n);

#endif /* HELIX_KSTRING_H */
