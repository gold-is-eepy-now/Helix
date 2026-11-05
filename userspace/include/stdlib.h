/* stdlib.h — memory allocation, process exit and number parsing. */
#ifndef HELIX_LIBC_STDLIB_H
#define HELIX_LIBC_STDLIB_H

#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

void *malloc(size_t n);
void *calloc(size_t count, size_t size);
void *realloc(void *p, size_t n);
void free(void *p);

__attribute__((noreturn)) void exit(int status);
__attribute__((noreturn)) void abort(void);

long strtol(const char *s, char **end, int base);
int atoi(const char *s);

/* There is no environment yet: getenv always finds nothing. */
char *getenv(const char *name);

#endif
