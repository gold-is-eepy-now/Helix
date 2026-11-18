/* trust_roots.h — the built-in root certificates (DER), generated into trust_roots.c
 * by tools/gen-trust-roots.py. */
#ifndef HELIX_TRUST_ROOTS_H
#define HELIX_TRUST_ROOTS_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const uint8_t *der;
    size_t len;
} trust_root_t;

extern const trust_root_t trust_roots[];
extern const size_t trust_root_count;

#endif /* HELIX_TRUST_ROOTS_H */
