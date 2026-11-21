/* The memory routines the Cell runtime provides (user/ulib.c). */
#ifndef LOOM_USER_STRING_H
#define LOOM_USER_STRING_H
#include <stddef.h>
void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
#endif
