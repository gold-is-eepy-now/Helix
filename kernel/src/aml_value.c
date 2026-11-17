/* aml_value.c — value storage. Temporaries live in an arena that is released when a top-level
 * evaluation finishes; values kept in the namespace, and results handed to callers, are
 * copied to the heap and freed explicitly. */
#include "aml_internal.h"
#include "heap.h"
#include "kstring.h"

/* Each arena block starts with a header holding the link to the previous block. */
typedef struct arena_block {
    struct arena_block *prev;
    unsigned char pad[8];        /* keep the payload 16-byte aligned */
} arena_block_t;

static arena_block_t *g_arena_top;

void *aml_arena_alloc(size_t n) {
    arena_block_t *b = kmalloc(sizeof(arena_block_t) + n);
    if (b == NULL) return NULL;
    b->prev = g_arena_top;
    g_arena_top = b;
    return (uint8_t *)b + sizeof(arena_block_t);
}

void *aml_arena_mark(void) { return g_arena_top; }

void aml_arena_release(void *mark) {
    while (g_arena_top != mark && g_arena_top) {
        arena_block_t *prev = g_arena_top->prev;
        kfree(g_arena_top);
        g_arena_top = prev;
    }
}

static void *alloc_for(size_t n, bool heap) {
    if (n == 0) n = 1;
    return heap ? kmalloc(n) : aml_arena_alloc(n);
}

uint64_t aml_mask(uint64_t v) {
    return aml_revision < 2 ? (v & 0xFFFFFFFFu) : v;
}

/* Deep copy. Packages copy their elements; references copy the pointer. */
aml_status_t aml_value_copy(aml_value_t *dst, const aml_value_t *src, bool heap) {
    memset(dst, 0, sizeof(*dst));
    dst->type = src->type;
    switch (src->type) {
    case AML_T_NONE:
        return AML_OK;
    case AML_T_INT:
        dst->integer = src->integer;
        return AML_OK;
    case AML_T_STR:
    case AML_T_BUF:
        dst->data = alloc_for(src->len, heap);
        if (dst->data == NULL) return AML_ERR_HARDWARE;
        if (src->len) memcpy(dst->data, src->data, src->len);
        dst->len = src->len;
        return AML_OK;
    case AML_T_PKG: {
        dst->elems = alloc_for(sizeof(aml_value_t) * (src->count ? src->count : 1), heap);
        if (dst->elems == NULL) return AML_ERR_HARDWARE;
        dst->count = src->count;
        for (uint32_t i = 0; i < src->count; i++) {
            aml_status_t st = aml_value_copy(&dst->elems[i], &src->elems[i], heap);
            if (st != AML_OK) return st;
        }
        return AML_OK;
    }
    case AML_T_REF:
        /* A deferred reference carries its placeholder in data; the copy shares it, and
         * aml_resolve_references frees it. */
        dst->ref = src->ref;
        dst->data = src->data;
        return AML_OK;
    default:
        return AML_ERR_TYPE;
    }
}

/* Free a heap value and everything it owns. Arena values are never passed here. */
void aml_value_destroy(aml_value_t *v) {
    switch (v->type) {
    case AML_T_STR:
    case AML_T_BUF:
        kfree(v->data);
        break;
    case AML_T_PKG:
        for (uint32_t i = 0; i < v->count; i++) aml_value_destroy(&v->elems[i]);
        kfree(v->elems);
        break;
    default:
        break;
    }
    memset(v, 0, sizeof(*v));
}

/* Integer view of a value. Buffers and strings give their first eight bytes (little-endian
 * for buffers; for strings, decimal or 0x-prefixed hex, as ACPI's ToInteger defines). */
aml_status_t aml_to_int(const aml_value_t *v, uint64_t *out) {
    switch (v->type) {
    case AML_T_INT:
        *out = aml_mask(v->integer);
        return AML_OK;
    case AML_T_BUF: {
        uint64_t x = 0;
        for (uint32_t i = 0; i < v->len && i < 8; i++) x |= (uint64_t)v->data[i] << (8 * i);
        *out = aml_mask(x);
        return AML_OK;
    }
    case AML_T_STR: {
        uint64_t x = 0;
        const char *p = (const char *)v->data;
        if (p == NULL) return AML_ERR_TYPE;
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
            for (p += 2; *p; p++) {
                char c = *p;
                unsigned d;
                if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
                else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
                else break;
                x = x * 16 + d;
            }
        } else {
            for (; *p >= '0' && *p <= '9'; p++) x = x * 10 + (uint64_t)(*p - '0');
        }
        *out = aml_mask(x);
        return AML_OK;
    }
    default:
        return AML_ERR_TYPE;
    }
}
