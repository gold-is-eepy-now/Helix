/* stdlib.c — allocation (on the program break, via libhelix), exit and number
 * parsing (see include/stdlib.h). */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "libhelix.h"

void *malloc(size_t n) {
    void *p = helix_malloc(n);
    if (p == NULL) errno = ENOMEM;
    return p;
}

void *calloc(size_t count, size_t size) {
    if (size != 0 && count > (size_t)-1 / size) {
        errno = ENOMEM;
        return NULL;
    }
    size_t total = count * size;
    void *p = malloc(total);
    if (p != NULL) memset(p, 0, total);
    return p;
}

void *realloc(void *p, size_t n) {
    if (p == NULL) return malloc(n);
    size_t old = helix_msize(p);
    void *q = malloc(n);
    if (q == NULL) return NULL;
    memcpy(q, p, old < n ? old : n);
    helix_free_mem(p);
    return q;
}

void free(void *p) {
    helix_free_mem(p);
}

void exit(int status) {
    helix_exit(status);
}

void abort(void) {
    helix_exit(134);
}

/* Parses an optional sign, then digits in `base` (2..36; 0 means 10, or 16 with a
 * 0x prefix). Stops at the first character that is not a digit. */
long strtol(const char *s, char **end, int base) {
    const char *p = s;
    int neg = 0;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '-') { neg = 1; p++; } else if (*p == '+') { p++; }
    if (base == 0) {
        base = 10;
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; p += 2; }
    }
    long v = 0;
    int any = 0;
    for (;; p++) {
        int d;
        if (*p >= '0' && *p <= '9') d = *p - '0';
        else if (*p >= 'a' && *p <= 'z') d = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'Z') d = *p - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v * base + d;
        any = 1;
    }
    if (end != NULL) *end = (char *)(any ? p : s);
    return neg ? -v : v;
}

int atoi(const char *s) {
    return (int)strtol(s, NULL, 10);
}

