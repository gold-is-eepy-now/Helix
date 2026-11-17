/* aml_load.c — walk a definition block and build the namespace from its declarations.
 *
 * Declarations are handled here: Name, Scope, Device, Processor, PowerResource, ThermalZone,
 * Method, Alias, OperationRegion, Field, IndexField, BankField, Mutex, Event and External.
 * Method bodies are kept as bytecode and run later, by the interpreter. Values that a
 * declaration needs straight away (a Name's value, a region's offset and length) are
 * evaluated here, with the interpreter. */
#include "aml_internal.h"
#include "kprintf_compat.h"
#include "heap.h"
#include "kstring.h"

static bool name_lead(uint8_t b) {
    return b == '\\' || b == '^' || b == '_' || (b >= 'A' && b <= 'Z') || b == 0x2E || b == 0x2F;
}

static void log_name(const char *what, const char name[4]) {
    kprintf("[aml] %s %c%c%c%c\n", what, name[0], name[1], name[2], name[3]);
}

/* Read an opcode: a one-byte opcode, or 0x5B followed by a second byte. */
static uint16_t read_opcode(aml_stream_t *s) {
    uint8_t b = aml_u8(s);
    if (b == 0x5B) return (uint16_t)(0x5B00u | aml_u8(s));
    return b;
}

static void peek_opcode(aml_stream_t *s, uint16_t *op, uint32_t *at) {
    uint32_t save = s->pos;
    *at = save;
    *op = read_opcode(s);
    s->pos = save;
    s->bad = false;
}

/* Evaluate one TermArg in the loader's scope and copy the result to the heap, so a Name can
 * keep it after the temporaries are released. */
static aml_status_t eval_to_heap(aml_node_t *scope, aml_stream_t *s, aml_value_t *out) {
    void *mark = aml_arena_mark();
    aml_value_t tmp;
    aml_status_t st = aml_eval_term_arg(scope, s, &tmp);
    if (st == AML_OK) st = aml_value_copy(out, &tmp, true);
    aml_arena_release(mark);
    return st;
}

static aml_status_t define(aml_node_t *scope, aml_stream_t *s, uint8_t kind, aml_node_t **out) {
    aml_path_t path;
    if (!aml_namestring(s, &path)) return AML_ERR_BAD_CODE;
    return aml_ns_define(scope, &path, kind, out);
}

static aml_status_t load_method(aml_node_t *scope, aml_stream_t *s) {
    uint32_t end = aml_pkg_end(s);
    aml_path_t path;
    if (s->bad || !aml_namestring(s, &path)) return AML_ERR_BAD_CODE;
    uint8_t flags = aml_u8(s);
    if (s->bad || end > s->end) return AML_ERR_BAD_CODE;
    aml_node_t *m;
    aml_status_t st = aml_ns_define(scope, &path, NK_METHOD, &m);
    if (st != AML_OK) return st;
    m->u.method.code = s->base + s->pos;
    m->u.method.len = end - s->pos;
    m->u.method.argc = flags & 7;
    s->pos = end;
    return AML_OK;
}

/* Field list: named elements (NameSeg PkgLength), reserved gaps (0x00), access and connection
 * descriptors (0x01 to 0x03). Only the bit layout is kept; access width, lock and update rules
 * are read past but not honoured yet (docs/24-aml.md). */
static aml_status_t load_field_list(aml_node_t *scope, aml_stream_t *s, uint32_t end, aml_node_t *region,
                                    field_kind_t kind, aml_node_t *index_reg, aml_node_t *data_reg,
                                    aml_node_t *bank_reg, uint64_t bank_value, uint64_t bit_pos) {
    while (!s->bad && s->pos < end) {
        uint8_t b = aml_peek(s);
        if (b == 0x00) {                          /* ReservedField: skip bits */
            s->pos++;
            bit_pos += aml_pkglen(s);
        } else if (b == 0x01) {                   /* AccessField: type and attributes */
            s->pos += 3;
        } else if (b == 0x02) {                   /* ConnectionField: a buffer or a name */
            s->pos++;
            aml_value_t ignored;
            aml_status_t st = aml_eval_term_arg(scope, s, &ignored);
            if (st != AML_OK) return st;
            aml_value_destroy(&ignored);
        } else if (b == 0x03) {                   /* ExtendedAccessField: type, attrib, length */
            s->pos += 4;
        } else {
            char name[4];
            if (!aml_nameseg(s, name)) {
                kprintf("[aml] field element at byte %u is not a name: %x %x %x %x\n", (unsigned)s->pos,
                        (unsigned)s->base[s->pos], (unsigned)s->base[s->pos + 1],
                        (unsigned)s->base[s->pos + 2], (unsigned)s->base[s->pos + 3]);
                return AML_ERR_BAD_CODE;
            }
            uint32_t bits = aml_pkglen(s);
            aml_path_t path = { .nseg = 1 };
            memcpy(path.seg[0], name, 4);
            aml_node_t *f;
            aml_status_t st = aml_ns_define(scope, &path, NK_FIELD, &f);
            if (st != AML_OK) return st;
            f->u.field.kind = kind;
            f->u.field.region = region;
            f->u.field.index_reg = index_reg;
            f->u.field.data_reg = data_reg;
            f->u.field.bank_reg = bank_reg;
            f->u.field.bank_value = bank_value;
            f->u.field.bit_off = bit_pos;
            f->u.field.bit_len = bits;
            bit_pos += bits;
        }
    }
    return s->bad ? AML_ERR_BAD_CODE : AML_OK;
}

static aml_node_t *find_named(aml_node_t *scope, aml_stream_t *s) {
    aml_path_t path;
    if (!aml_namestring(s, &path)) return NULL;
    return aml_ns_lookup(scope, &path);
}

static aml_status_t load_field(aml_node_t *scope, aml_stream_t *s, uint16_t op) {
    uint32_t end = aml_pkg_end(s);
    if (s->bad || end > s->end) return AML_ERR_BAD_CODE;
    if (op == 0x5B81) {                            /* Field */
        aml_node_t *region = find_named(scope, s);
        if (region == NULL || region->kind != NK_REGION) return AML_ERR_NOT_FOUND;
        aml_u8(s);                                 /* flags: access, lock, update (not used yet) */
        return load_field_list(scope, s, end, region, FK_SIMPLE, NULL, NULL, NULL, 0, 0);
    }
    if (op == 0x5B86) {                            /* IndexField */
        aml_node_t *index_reg = find_named(scope, s);
        aml_node_t *data_reg = find_named(scope, s);
        if (!index_reg || !data_reg) return AML_ERR_NOT_FOUND;
        aml_u8(s);
        return load_field_list(scope, s, end, NULL, FK_INDEX, index_reg, data_reg, NULL, 0, 0);
    }
    /* BankField: region, bank register, bank value (a TermArg), flags, list. */
    aml_node_t *region = find_named(scope, s);
    aml_node_t *bank = find_named(scope, s);
    if (!region || !bank || region->kind != NK_REGION) return AML_ERR_NOT_FOUND;
    aml_value_t bank_value;
    aml_status_t st = aml_eval_term_arg(scope, s, &bank_value);
    if (st != AML_OK) return st;
    uint64_t bv = 0;
    st = aml_to_int(&bank_value, &bv);
    if (st != AML_OK) return st;
    aml_u8(s);
    return load_field_list(scope, s, end, region, FK_BANK, NULL, NULL, bank, bv, 0);
}

static aml_status_t load_region(aml_node_t *scope, aml_stream_t *s) {
    aml_node_t *r;
    aml_status_t st = define(scope, s, NK_REGION, &r);
    if (st != AML_OK) return st;
    r->u.region.space = aml_u8(s);
    aml_value_t off, len;
    st = aml_eval_term_arg(scope, s, &off);
    if (st != AML_OK) return st;
    st = aml_eval_term_arg(scope, s, &len);
    if (st != AML_OK) return st;
    uint64_t base = 0, length = 0;
    /* A region whose address or length cannot be worked out is kept but refused at access. */
    r->u.region.ok = aml_to_int(&off, &base) == AML_OK && aml_to_int(&len, &length) == AML_OK;
    r->u.region.base = base;
    r->u.region.len = length;
    aml_value_destroy(&off);
    aml_value_destroy(&len);
    return s->bad ? AML_ERR_BAD_CODE : AML_OK;
}

/* One declaration, then the next. Anything else at the top of a definition block is an error,
 * because the loader cannot tell how long an unknown object is. */
aml_status_t aml_load_terms(aml_node_t *scope, aml_stream_t *s) {
    while (!s->bad && s->pos < s->end) {
        uint16_t op;
        uint32_t at;
        peek_opcode(s, &op, &at);
        aml_status_t st = AML_OK;
        switch (op) {
        case 0x08: {                               /* Name */
            read_opcode(s);
            aml_node_t *n;
            st = define(scope, s, NK_NAME, &n);
            if (st != AML_OK) break;
            /* ACPI allows a Name's value to be a data object or a reference to another name,
             * not an expression. A reference may point at a name defined later in the table;
             * that is logged and left empty rather than failing the whole load. */
            if (name_lead(aml_peek(s))) {
                aml_path_t target;
                if (!aml_namestring(s, &target)) { st = AML_ERR_BAD_CODE; break; }
                aml_node_t *t = aml_ns_lookup(scope, &target);
                if (t) {
                    n->value.type = AML_T_REF;
                    n->value.ref = t;
                } else {
                    st = aml_make_deferred_ref(scope, &target, &n->value);
                }
                break;
            }
            st = eval_to_heap(scope, s, &n->value);
            if (st != AML_OK) log_name("Name value failed:", n->name);
            break;
        }
        case 0x10: {                               /* Scope */
            read_opcode(s);
            uint32_t end = aml_pkg_end(s);
            aml_path_t path;
            if (s->bad || end > s->end || !aml_namestring(s, &path)) { st = AML_ERR_BAD_CODE; break; }
            aml_node_t *sc = aml_ns_lookup(scope, &path);
            if (sc == NULL) {
                aml_path_t prefix = path;
                prefix.nseg = (uint8_t)(path.nseg - 1);
                aml_node_t *parent = aml_ns_lookup(scope, &prefix);
                if (parent == NULL || path.nseg == 0) { st = AML_ERR_NOT_FOUND; break; }
                sc = aml_ns_new(parent, path.seg[path.nseg - 1], NK_SCOPE);
                if (sc == NULL) { st = AML_ERR_NOT_LOADED; break; }
            }
            uint32_t save = s->end;
            s->end = end;
            st = aml_load_terms(sc, s);
            s->end = save;
            s->pos = end;
            break;
        }
        case 0x14:                                 /* Method */
            read_opcode(s);
            st = load_method(scope, s);
            break;
        case 0x06: {                               /* Alias */
            read_opcode(s);
            aml_path_t src, dst;
            if (!aml_namestring(s, &src) || !aml_namestring(s, &dst)) { st = AML_ERR_BAD_CODE; break; }
            aml_node_t *target = aml_ns_lookup(scope, &src);
            if (target == NULL) { st = AML_ERR_NOT_FOUND; break; }
            aml_node_t *a;
            st = aml_ns_define(scope, &dst, NK_ALIAS, &a);
            if (st == AML_OK) a->u.alias.target = target;
            break;
        }
        case 0x15: {                               /* External: a declaration for another table */
            read_opcode(s);
            aml_path_t p;
            if (!aml_namestring(s, &p)) { st = AML_ERR_BAD_CODE; break; }
            aml_u8(s);
            aml_u8(s);
            break;
        }
        case 0x5B80:                               /* OperationRegion */
            read_opcode(s);
            st = load_region(scope, s);
            break;
        case 0x5B81:
        case 0x5B86:
        case 0x5B87:                               /* Field, IndexField, BankField */
            read_opcode(s);
            st = load_field(scope, s, op);
            break;
        case 0x5B01: {                             /* Mutex */
            read_opcode(s);
            aml_node_t *m;
            st = define(scope, s, NK_MUTEX, &m);
            if (st == AML_OK) aml_u8(s);
            break;
        }
        case 0x5B02:                               /* Event */
            read_opcode(s);
            st = define(scope, s, NK_EVENT, &(aml_node_t *){ 0 });
            break;
        case 0x5B82:                               /* Device */
        case 0x5B83:                               /* Processor */
        case 0x5B84:                               /* PowerResource */
        case 0x5B85: {                             /* ThermalZone */
            read_opcode(s);
            uint32_t end = aml_pkg_end(s);
            if (s->bad || end > s->end) { st = AML_ERR_BAD_CODE; break; }
            uint8_t kind = op == 0x5B82 ? NK_DEVICE : op == 0x5B83 ? NK_PROCESSOR
                         : op == 0x5B84 ? NK_POWER : NK_THERMAL;
            aml_node_t *n;
            st = define(scope, s, kind, &n);
            if (st != AML_OK) break;
            if (op == 0x5B83) {                    /* ProcID, PBlk address, PBlk length */
                aml_u8(s); aml_u32(s); aml_u8(s);
            } else if (op == 0x5B84) {             /* SystemLevel, ResourceOrder */
                aml_u8(s); aml_u16(s);
            }
            uint32_t save = s->end;
            s->end = end;
            st = aml_load_terms(n, s);
            s->end = save;
            s->pos = end;
            break;
        }
        default:
            aml_log_unsupported(op, at);
            return AML_ERR_UNSUPPORTED;
        }
        if (st != AML_OK) {
            kprintf("[aml] stopped at byte %u (opcode 0x%x): %s\n", (unsigned)at, (unsigned)op,
                    aml_status_name(st));
            return st;
        }
    }
    return s->bad ? AML_ERR_BAD_CODE : AML_OK;
}

void aml_log_unsupported(uint16_t opcode, uint32_t pos) {
    kprintf("[aml] unsupported opcode 0x%x at byte %u\n", (unsigned)opcode, (unsigned)pos);
}

aml_status_t aml_make_deferred_ref(aml_node_t *scope, const aml_path_t *path, aml_value_t *out) {
    deferred_ref_t *d = kmalloc(sizeof(*d));
    if (d == NULL) return AML_ERR_HARDWARE;
    d->scope = scope;
    d->path = *path;
    memset(out, 0, sizeof(*out));
    out->type = AML_T_REF;
    out->ref = NULL;
    out->data = (uint8_t *)d;
    return AML_OK;
}

static void resolve_value(aml_value_t *v) {
    if (v->type == AML_T_REF && v->ref == NULL && v->data) {
        deferred_ref_t *d = (deferred_ref_t *)v->data;
        aml_node_t *t = aml_ns_lookup(d->scope, &d->path);
        if (t) {
            v->ref = t;
            kfree(d);
            v->data = NULL;
        }
    } else if (v->type == AML_T_PKG) {
        for (uint32_t i = 0; i < v->count; i++) resolve_value(&v->elems[i]);
    }
}

static void resolve_tree(aml_node_t *n) {
    if (n->kind == NK_NAME) resolve_value(&n->value);
    for (aml_node_t *c = n->child; c; c = c->next) resolve_tree(c);
}

aml_status_t aml_resolve_references(void) {
    resolve_tree(aml_ns_root());
    return AML_OK;
}
