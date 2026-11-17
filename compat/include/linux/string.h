/* linux/string.h — the string helpers drivers use, from Helix's freestanding library. */
#ifndef HELIX_COMPAT_LINUX_STRING_H
#define HELIX_COMPAT_LINUX_STRING_H
#include <linux/types.h>
#include "kstring.h"
void *memchr(const void *s, int c, size_t n);
#endif
