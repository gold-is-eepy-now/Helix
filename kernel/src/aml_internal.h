/* aml_internal.h — shared types for the AML interpreter. Not for use outside kernel/src/aml*.c. */
#ifndef HELIX_AML_INTERNAL_H
#define HELIX_AML_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "aml.h"

typedef enum {
    NK_ROOT, NK_SCOPE, NK_DEVICE, NK_PROCESSOR, NK_POWER, NK_THERMAL,
    NK_METHOD, NK_NAME, NK_REGION, NK_FIELD, NK_MUTEX, NK_EVENT, NK_ALIAS,
} aml_kind_t;

typedef enum { FK_SIMPLE, FK_INDEX, FK_BANK } field_kind_t;

struct aml_node {
    char name[4];
    uint8_t kind;             /* aml_kind_t */
    aml_node_t *parent;
    aml_node_t *child;        /* first child */
    aml_node_t *next;         /* next sibling */
    aml_value_t value;        /* NK_NAME: the object, owned by the heap */
    union {
        struct { const uint8_t *code; uint32_t len; uint8_t argc; } method;
        struct { uint8_t space; bool ok; uint64_t base; uint64_t len; } region;
        struct {
            field_kind_t kind;
            aml_node_t *region;      /* FK_SIMPLE, FK_BANK: the operation region */
            aml_node_t *index_reg;   /* FK_INDEX */
            aml_node_t *data_reg;    /* FK_INDEX */
            aml_node_t *bank_reg;    /* FK_BANK */
            uint64_t bank_value;     /* FK_BANK */
            uint64_t bit_off;        /* from the start of the region (or index space) */
            uint32_t bit_len;
        } field;
        struct { aml_node_t *target; } alias;
    } u;
};

/* A name as written in bytecode or in a path string: up to 16 segments. */
typedef struct {
    bool root;               /* leading '\' */
    uint8_t parents;         /* count of leading '^' */
    uint8_t nseg;
    char seg[16][4];
} aml_path_t;

/* A byte cursor over one block of bytecode. `bad` is set on any overrun. */
typedef struct {
    const uint8_t *base;
    uint32_t pos;
    uint32_t end;
    bool bad;
} aml_stream_t;

/* Interpreter-wide state (aml_ns.c). */
extern uint8_t aml_revision;     /* table revision of the block being run; <2 means 32-bit */
extern uint32_t aml_budget;      /* steps left for the current top-level evaluation */
extern unsigned aml_depth;       /* method call depth */
extern bool aml_loading;         /* true while a definition block is being loaded */

/* ---- namespace (aml_ns.c) ---- */
aml_node_t *aml_ns_root(void);
aml_node_t *aml_ns_child(aml_node_t *parent, const char name[4]);
aml_node_t *aml_ns_new(aml_node_t *parent, const char name[4], uint8_t kind);
aml_node_t *aml_ns_lookup(aml_node_t *scope, const aml_path_t *path);
aml_status_t aml_ns_define(aml_node_t *scope, const aml_path_t *path, uint8_t kind, aml_node_t **out);
void aml_ns_path_text(const aml_node_t *node, char *buf, unsigned size);
bool aml_path_from_text(const char *text, aml_path_t *out);
void aml_ns_reset(void);

/* ---- values (aml_value.c) ---- */
void *aml_arena_alloc(size_t n);                 /* freed by aml_arena_release */
void *aml_arena_mark(void);
void aml_arena_release(void *mark);
aml_status_t aml_value_copy(aml_value_t *dst, const aml_value_t *src, bool heap);
void aml_value_destroy(aml_value_t *v);          /* heap values only */
aml_status_t aml_to_int(const aml_value_t *v, uint64_t *out);
uint64_t aml_mask(uint64_t v);                   /* clamp to the table revision's width */

/* ---- byte stream and name parsing (aml_parse.c) ---- */
uint8_t aml_peek(aml_stream_t *s);
uint8_t aml_u8(aml_stream_t *s);
uint16_t aml_u16(aml_stream_t *s);
uint32_t aml_u32(aml_stream_t *s);
uint64_t aml_u64(aml_stream_t *s);
uint32_t aml_pkglen(aml_stream_t *s);            /* returns the length, not the end */
uint32_t aml_pkg_end(aml_stream_t *s);           /* the end offset of the object */
bool aml_namestring(aml_stream_t *s, aml_path_t *out);
bool aml_nameseg(aml_stream_t *s, char out[4]);

/* ---- expression and statement interpreter (aml_exec.c) ---- */
aml_status_t aml_eval_term_arg(aml_node_t *scope, aml_stream_t *s, aml_value_t *out);
aml_status_t aml_value_of_node(aml_node_t *node, aml_value_t *out);
aml_status_t aml_store_node(aml_node_t *node, const aml_value_t *value);
aml_status_t aml_invoke(aml_node_t *method, const aml_value_t *args, unsigned argc, aml_value_t *out);

/* ---- acpi.c hooks ---- */
const void *acpi_table_data(unsigned index);
uint64_t acpi_hhdm(void);

/* ---- fields and operation regions (aml_field.c) ---- */
aml_status_t aml_field_read(aml_node_t *field, aml_value_t *out);
aml_status_t aml_field_write(aml_node_t *field, const aml_value_t *value);

/* ---- region backend (aml_region.c). The host unit test provides its own. ---- */
aml_status_t aml_region_read8(uint8_t space, uint64_t addr, uint8_t *out);
aml_status_t aml_region_write8(uint8_t space, uint64_t addr, uint8_t value);

/* ---- definition loading (aml_load.c) ----
 * While loading, a name in a Name's value or a package that is not defined yet becomes a
 * deferred reference (ref NULL, data = a deferred_ref_t). aml_resolve_references fills them in
 * once every block is in the namespace. */
typedef struct { aml_node_t *scope; aml_path_t path; } deferred_ref_t;
aml_status_t aml_load_terms(aml_node_t *scope, aml_stream_t *s);
aml_status_t aml_resolve_references(void);
aml_status_t aml_make_deferred_ref(aml_node_t *scope, const aml_path_t *path, aml_value_t *out);

/* Diagnostics. */
void aml_log_unsupported(uint16_t opcode, uint32_t pos);

#endif /* HELIX_AML_INTERNAL_H */
