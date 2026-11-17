/* aml_exec.c — the AML interpreter: statements (If, While, Return), expressions (arithmetic,
 * logic, comparison, Store, Index, and so on), method calls, and Sleep, Stall and the sync
 * operations. Every evaluation spends from a step budget, so a firmware loop that never ends
 * is reported as AML_ERR_BUDGET instead of hanging the kernel.
 *
 * Operands are evaluated left to right, then the optional target, as ACPI defines. Anything
 * not listed in the switch statements returns AML_ERR_UNSUPPORTED and is logged. */
#include "aml_internal.h"
#include "kprintf_compat.h"
#include "kstring.h"
#include "time.h"

#define MAX_DEPTH 8
#define STEP_BUDGET 500000u

typedef struct {
    aml_value_t local[8];
    aml_value_t arg[7];
    aml_value_t ret;
    bool has_ret;
    aml_node_t *method;
} frame_t;

typedef enum { CTL_NORMAL, CTL_RETURN, CTL_BREAK, CTL_CONTINUE } ctl_t;
typedef enum { TG_NONE, TG_DEBUG, TG_SLOT, TG_NODE } tg_kind_t;

typedef struct {
    tg_kind_t kind;
    aml_value_t *slot;
    aml_node_t *node;
} target_t;

static aml_status_t eval_arg(aml_node_t *scope, frame_t *f, aml_stream_t *s, aml_value_t *out);
static aml_status_t exec_block(aml_node_t *scope, frame_t *f, aml_stream_t *s, uint32_t end, ctl_t *ctl);

static aml_status_t set_int(aml_value_t *out, uint64_t v) {
    memset(out, 0, sizeof(*out));
    out->type = AML_T_INT;
    out->integer = aml_mask(v);
    return AML_OK;
}

static bool name_lead(uint8_t b) {
    return b == '\\' || b == '^' || b == '_' || (b >= 'A' && b <= 'Z') || b == 0x2E || b == 0x2F;
}

static aml_status_t spend(void) {
    if (aml_budget == 0) return AML_ERR_BUDGET;
    aml_budget--;
    return AML_OK;
}

static aml_status_t eval_int(aml_node_t *scope, frame_t *f, aml_stream_t *s, uint64_t *out) {
    aml_value_t v;
    aml_status_t st = eval_arg(scope, f, s, &v);
    if (st != AML_OK) return st;
    return aml_to_int(&v, out);
}

/* ---- targets: where a result is stored ---- */

static aml_status_t parse_target(aml_node_t *scope, frame_t *f, aml_stream_t *s, target_t *t) {
    memset(t, 0, sizeof(*t));
    uint8_t b = aml_peek(s);
    if (s->bad) return AML_ERR_BAD_CODE;
    if (b == 0x00) {                            /* NullName: discard the result */
        s->pos++;
        t->kind = TG_NONE;
        return AML_OK;
    }
    if (b == 0x5B && s->pos + 1 < s->end && s->base[s->pos + 1] == 0x31) {   /* Debug */
        s->pos += 2;
        t->kind = TG_DEBUG;
        return AML_OK;
    }
    if (b >= 0x60 && b <= 0x67) {
        s->pos++;
        t->kind = TG_SLOT;
        t->slot = &f->local[b - 0x60];
        return AML_OK;
    }
    if (b >= 0x68 && b <= 0x6E) {
        s->pos++;
        t->kind = TG_SLOT;
        t->slot = &f->arg[b - 0x68];
        return AML_OK;
    }
    if (name_lead(b)) {
        aml_path_t path;
        if (!aml_namestring(s, &path)) return AML_ERR_BAD_CODE;
        aml_node_t *n = aml_ns_lookup(scope, &path);
        while (n && n->kind == NK_ALIAS) n = n->u.alias.target;
        if (n == NULL) return AML_ERR_NOT_FOUND;
        t->kind = TG_NODE;
        t->node = n;
        return AML_OK;
    }
    aml_log_unsupported(b, s->pos);
    return AML_ERR_UNSUPPORTED;
}

static aml_status_t target_store(const target_t *t, const aml_value_t *v) {
    switch (t->kind) {
    case TG_NONE:
    case TG_DEBUG:
        return AML_OK;
    case TG_SLOT: {
        aml_value_t tmp;
        aml_status_t st = aml_value_copy(&tmp, v, false);
        if (st != AML_OK) return st;
        *t->slot = tmp;
        return AML_OK;
    }
    case TG_NODE:
        return aml_store_node(t->node, v);
    }
    return AML_ERR_TYPE;
}

static aml_status_t target_read(const target_t *t, aml_value_t *out) {
    switch (t->kind) {
    case TG_SLOT:
        return aml_value_copy(out, t->slot, false);
    case TG_NODE:
        return aml_value_of_node(t->node, out);
    default:
        return AML_ERR_TYPE;
    }
}

/* Store a value and return it as the expression's result (Store, CopyObject). */
static aml_status_t finish_store(const target_t *t, const aml_value_t *v, aml_value_t *out) {
    aml_status_t st = target_store(t, v);
    if (st != AML_OK) return st;
    return aml_value_copy(out, v, false);
}

/* ---- names and objects ---- */

aml_status_t aml_value_of_node(aml_node_t *node, aml_value_t *out) {
    while (node && node->kind == NK_ALIAS) node = node->u.alias.target;
    if (node == NULL) return AML_ERR_NOT_FOUND;
    switch (node->kind) {
    case NK_NAME:
        return aml_value_copy(out, &node->value, false);
    case NK_FIELD:
        return aml_field_read(node, out);
    case NK_METHOD: {
        aml_status_t st = aml_invoke(node, NULL, 0, out);
        return st;
    }
    default:
        memset(out, 0, sizeof(*out));
        out->type = AML_T_REF;
        out->ref = node;
        return AML_OK;
    }
}

aml_status_t aml_store_node(aml_node_t *node, const aml_value_t *value) {
    while (node && node->kind == NK_ALIAS) node = node->u.alias.target;
    if (node == NULL) return AML_ERR_NOT_FOUND;
    switch (node->kind) {
    case NK_NAME: {
        aml_value_t tmp;
        aml_status_t st = aml_value_copy(&tmp, value, true);
        if (st != AML_OK) return st;
        aml_value_destroy(&node->value);
        node->value = tmp;
        return AML_OK;
    }
    case NK_FIELD:
        return aml_field_write(node, value);
    default:
        return AML_ERR_TYPE;
    }
}

aml_status_t aml_invoke(aml_node_t *method, const aml_value_t *args, unsigned argc, aml_value_t *out) {
    if (aml_depth >= MAX_DEPTH) return AML_ERR_DEPTH;
    frame_t fr;
    memset(&fr, 0, sizeof(fr));
    fr.method = method;
    for (unsigned i = 0; i < argc && i < 7; i++) {
        aml_status_t st = aml_value_copy(&fr.arg[i], &args[i], false);
        if (st != AML_OK) return st;
    }
    aml_depth++;
    aml_stream_t s = { method->u.method.code, 0, method->u.method.len, false };
    ctl_t ctl = CTL_NORMAL;
    aml_status_t st = exec_block(method->parent, &fr, &s, method->u.method.len, &ctl);
    aml_depth--;
    if (st != AML_OK) return st;
    if (fr.has_ret) {
        *out = fr.ret;
    } else {
        memset(out, 0, sizeof(*out));
    }
    return AML_OK;
}

/* A name in an expression: a method call (with its arguments parsed from the stream), or the
 * value of the object. */
static aml_status_t eval_name(aml_node_t *scope, frame_t *f, aml_stream_t *s, aml_value_t *out) {
    aml_path_t path;
    if (!aml_namestring(s, &path)) return AML_ERR_BAD_CODE;
    aml_node_t *n = aml_ns_lookup(scope, &path);
    while (n && n->kind == NK_ALIAS) n = n->u.alias.target;
    if (n == NULL) return AML_ERR_NOT_FOUND;
    if (n->kind == NK_METHOD) {
        unsigned argc = n->u.method.argc;
        aml_value_t args[7];
        for (unsigned i = 0; i < argc; i++) {
            aml_status_t st = eval_arg(scope, f, s, &args[i]);
            if (st != AML_OK) return st;
        }
        return aml_invoke(n, args, argc, out);
    }
    return aml_value_of_node(n, out);
}

/* ---- comparisons ---- */

static int compare(const aml_value_t *a, const aml_value_t *b, aml_status_t *st) {
    *st = AML_OK;
    if ((a->type == AML_T_STR && b->type == AML_T_STR) || (a->type == AML_T_BUF && b->type == AML_T_BUF)) {
        uint32_t n = a->len < b->len ? a->len : b->len;
        for (uint32_t i = 0; i < n; i++) {
            if (a->data[i] != b->data[i]) return a->data[i] < b->data[i] ? -1 : 1;
        }
        return a->len == b->len ? 0 : (a->len < b->len ? -1 : 1);
    }
    uint64_t x, y;
    *st = aml_to_int(a, &x);
    if (*st != AML_OK) return 0;
    *st = aml_to_int(b, &y);
    if (*st != AML_OK) return 0;
    return x == y ? 0 : (x < y ? -1 : 1);
}

/* ---- binary and unary operators with an optional result target ---- */

static aml_status_t binop(aml_node_t *scope, frame_t *f, aml_stream_t *s, uint16_t op, aml_value_t *out) {
    uint64_t x, y;
    aml_status_t st = eval_int(scope, f, s, &x);
    if (st != AML_OK) return st;
    st = eval_int(scope, f, s, &y);
    if (st != AML_OK) return st;
    target_t t;
    st = parse_target(scope, f, s, &t);
    if (st != AML_OK) return st;
    uint64_t r = 0;
    switch (op) {
    case 0x72: r = x + y; break;                        /* Add */
    case 0x74: r = x - y; break;                        /* Subtract */
    case 0x77: r = x * y; break;                        /* Multiply */
    case 0x79: r = y >= 64 ? 0 : x << y; break;         /* ShiftLeft */
    case 0x7A: r = y >= 64 ? 0 : x >> y; break;         /* ShiftRight */
    case 0x7B: r = x & y; break;                        /* And */
    case 0x7C: r = ~(x & y); break;                     /* NAnd */
    case 0x7D: r = x | y; break;                        /* Or */
    case 0x7E: r = ~(x | y); break;                     /* NOr */
    case 0x7F: r = x ^ y; break;                        /* Xor */
    case 0x85:                                          /* Mod */
        if (y == 0) return AML_ERR_RANGE;
        r = x % y;
        break;
    default:
        return AML_ERR_UNSUPPORTED;
    }
    aml_value_t v;
    set_int(&v, r);
    return finish_store(&t, &v, out);
}

static aml_status_t divide(aml_node_t *scope, frame_t *f, aml_stream_t *s, aml_value_t *out) {
    uint64_t x, y;
    aml_status_t st = eval_int(scope, f, s, &x);
    if (st != AML_OK) return st;
    st = eval_int(scope, f, s, &y);
    if (st != AML_OK) return st;
    if (y == 0) return AML_ERR_RANGE;
    target_t rem, quo;
    st = parse_target(scope, f, s, &rem);
    if (st != AML_OK) return st;
    st = parse_target(scope, f, s, &quo);
    if (st != AML_OK) return st;
    aml_value_t r, q;
    set_int(&r, x % y);
    set_int(&q, x / y);
    st = target_store(&rem, &r);
    if (st != AML_OK) return st;
    return finish_store(&quo, &q, out);
}

/* Store, Increment, Decrement, Not, FindSetLeft/RightBit, ToInteger: one operand and a target. */
static aml_status_t unop(aml_node_t *scope, frame_t *f, aml_stream_t *s, uint16_t op, aml_value_t *out) {
    target_t t;
    aml_status_t st;
    if (op == 0x75 || op == 0x76) {             /* Increment, Decrement: the operand is the target */
        st = parse_target(scope, f, s, &t);
        if (st != AML_OK) return st;
        aml_value_t cur;
        st = target_read(&t, &cur);
        if (st != AML_OK) return st;
        uint64_t x;
        st = aml_to_int(&cur, &x);
        if (st != AML_OK) return st;
        aml_value_t v;
        set_int(&v, op == 0x75 ? x + 1 : x - 1);
        return finish_store(&t, &v, out);
    }
    uint64_t x;
    st = eval_int(scope, f, s, &x);
    if (st != AML_OK) return st;
    st = parse_target(scope, f, s, &t);
    if (st != AML_OK) return st;
    uint64_t r = 0;
    switch (op) {
    case 0x80:                                  /* Not */
        r = ~x;
        break;
    case 0x81:                                  /* FindSetLeftBit: 1-based, 0 when no bit is set */
        for (unsigned i = 64; i-- > 0;) if ((x >> i) & 1) { r = i + 1; break; }
        break;
    case 0x82:                                  /* FindSetRightBit */
        for (unsigned i = 0; i < 64; i++) if ((x >> i) & 1) { r = i + 1; break; }
        break;
    case 0x99:                                  /* ToInteger */
        r = x;
        break;
    default:
        return AML_ERR_UNSUPPORTED;
    }
    aml_value_t v;
    set_int(&v, r);
    return finish_store(&t, &v, out);
}

/* ---- packages and buffers ---- */

static aml_status_t eval_buffer(aml_node_t *scope, frame_t *f, aml_stream_t *s, aml_value_t *out) {
    uint32_t end = aml_pkg_end(s);
    if (s->bad || end > s->end) return AML_ERR_BAD_CODE;
    uint64_t size;
    aml_status_t st = eval_int(scope, f, s, &size);
    if (st != AML_OK) return st;
    uint32_t avail = end - s->pos;
    uint32_t n = (uint32_t)size;
    if (n < avail) n = avail;                   /* a buffer is never shorter than its initializer */
    uint8_t *data = aml_arena_alloc(n ? n : 1);
    if (data == NULL) return AML_ERR_HARDWARE;
    memset(data, 0, n ? n : 1);
    memcpy(data, s->base + s->pos, avail);
    s->pos = end;
    memset(out, 0, sizeof(*out));
    out->type = AML_T_BUF;
    out->data = data;
    out->len = n;
    return AML_OK;
}

static aml_status_t eval_package(aml_node_t *scope, frame_t *f, aml_stream_t *s, bool variable, aml_value_t *out) {
    uint32_t end = 0;
    if (!variable) end = aml_pkg_end(s);
    if (s->bad) return AML_ERR_BAD_CODE;
    uint64_t count;
    aml_status_t st;
    if (variable) {
        st = eval_int(scope, f, s, &count);
        if (st != AML_OK) return st;
        end = s->end;
    } else {
        count = aml_u8(s);
    }
    if (end > s->end) return AML_ERR_BAD_CODE;
    aml_value_t *elems = aml_arena_alloc(sizeof(aml_value_t) * (count ? count : 1));
    if (elems == NULL) return AML_ERR_HARDWARE;
    for (uint64_t i = 0; i < count; i++) {
        memset(&elems[i], 0, sizeof(elems[i]));
        if (s->pos >= end) continue;            /* short list: the rest stay uninitialised */
        if (name_lead(aml_peek(s))) {
            aml_path_t path;
            if (!aml_namestring(s, &path)) return AML_ERR_BAD_CODE;
            aml_node_t *n = aml_ns_lookup(scope, &path);
            if (n == NULL) {
                if (!aml_loading) return AML_ERR_NOT_FOUND;
                st = aml_make_deferred_ref(scope, &path, &elems[i]);
                if (st != AML_OK) return st;
                continue;
            }
            elems[i].type = AML_T_REF;
            elems[i].ref = n;
        } else {
            st = eval_arg(scope, f, s, &elems[i]);
            if (st != AML_OK) return st;
        }
    }
    s->pos = end;
    memset(out, 0, sizeof(*out));
    out->type = AML_T_PKG;
    out->elems = elems;
    out->count = (uint32_t)count;
    return AML_OK;
}

static aml_status_t eval_string(aml_stream_t *s, aml_value_t *out) {
    uint32_t start = s->pos;
    while (s->pos < s->end && s->base[s->pos] != 0) s->pos++;
    if (s->pos >= s->end) return AML_ERR_BAD_CODE;
    uint32_t n = s->pos - start + 1;            /* include the NUL */
    s->pos++;
    uint8_t *data = aml_arena_alloc(n);
    if (data == NULL) return AML_ERR_HARDWARE;
    memcpy(data, s->base + start, n);
    memset(out, 0, sizeof(*out));
    out->type = AML_T_STR;
    out->data = data;
    out->len = n;
    return AML_OK;
}

/* ---- waiting: busy loops, since there is no scheduler sleep for firmware yet ---- */

static void busy_us(uint64_t us) {
    uint64_t per_ms = time_tsc_per_ms();
    if (per_ms == 0) return;
    uint64_t ticks = us * per_ms / 1000u;
    uint64_t start = time_tsc();
    while (time_tsc() - start < ticks) {
        __asm__ volatile("pause");
    }
}

/* Parse an operand that is a NameString and resolve it (for Acquire, Signal and so on). */
static aml_status_t named_object(aml_node_t *scope, aml_stream_t *s, aml_node_t **out) {
    aml_path_t path;
    if (!aml_namestring(s, &path)) return AML_ERR_BAD_CODE;
    aml_node_t *n = aml_ns_lookup(scope, &path);
    if (n == NULL) return AML_ERR_NOT_FOUND;
    *out = n;
    return AML_OK;
}

/* ---- expressions ---- */

static aml_status_t eval_extended(aml_node_t *scope, frame_t *f, aml_stream_t *s, uint16_t op, aml_value_t *out) {
    aml_status_t st;
    aml_node_t *n;
    switch (op) {
    case 0x5B21: {                              /* Stall: microseconds, at most 255 by spec */
        uint64_t us;
        st = eval_int(scope, f, s, &us);
        if (st != AML_OK) return st;
        busy_us(us);
        memset(out, 0, sizeof(*out));
        return AML_OK;
    }
    case 0x5B22: {                              /* Sleep: milliseconds */
        uint64_t ms;
        st = eval_int(scope, f, s, &ms);
        if (st != AML_OK) return st;
        busy_us(ms * 1000u);
        memset(out, 0, sizeof(*out));
        return AML_OK;
    }
    case 0x5B23:                                /* Acquire(mutex, timeout): FALSE means acquired */
        st = named_object(scope, s, &n);
        if (st != AML_OK) return st;
        aml_u16(s);
        return set_int(out, 0);
    case 0x5B25:                                /* Wait(event, timeout): 0 means signalled */
        st = named_object(scope, s, &n);
        if (st != AML_OK) return st;
        { uint64_t t; st = eval_int(scope, f, s, &t); if (st != AML_OK) return st; }
        return set_int(out, 0);
    case 0x5B24:                                /* Signal */
    case 0x5B26:                                /* Reset */
    case 0x5B27:                                /* Release */
        st = named_object(scope, s, &n);
        if (st != AML_OK) return st;
        memset(out, 0, sizeof(*out));
        return AML_OK;
    case 0x5B30:                                /* Revision */
        return set_int(out, 2);
    case 0x5B32: {                              /* Fatal(type, code, arg) */
        aml_u8(s);
        aml_u32(s);
        uint64_t arg;
        st = eval_int(scope, f, s, &arg);
        kprintf("[aml] Fatal() called by firmware (arg 0x%x)\n", (unsigned)arg);
        return st == AML_OK ? AML_ERR_FATAL : st;
    }
    case 0x5B12:                                /* CondRefOf */
    case 0x5B13:                                /* CreateField */
    case 0x5B20:                                /* Load */
    case 0x5B2A:                                /* Unload */
    case 0x5B33:                                /* Timer */
        aml_log_unsupported(op, s->pos);
        return AML_ERR_UNSUPPORTED;
    default:
        aml_log_unsupported(op, s->pos);
        return AML_ERR_UNSUPPORTED;
    }
}

static aml_status_t eval_arg(aml_node_t *scope, frame_t *f, aml_stream_t *s, aml_value_t *out) {
    aml_status_t st = spend();
    if (st != AML_OK) return st;
    memset(out, 0, sizeof(*out));
    uint8_t b = aml_peek(s);
    if (s->bad) return AML_ERR_BAD_CODE;

    /* Data objects and locals or arguments. */
    switch (b) {
    case 0x00: s->pos++; return set_int(out, 0);
    case 0x01: s->pos++; return set_int(out, 1);
    case 0xFF: s->pos++; return set_int(out, ~0ull);
    case 0x0A: s->pos++; return set_int(out, aml_u8(s));
    case 0x0B: s->pos++; return set_int(out, aml_u16(s));
    case 0x0C: s->pos++; return set_int(out, aml_u32(s));
    case 0x0E: s->pos++; return set_int(out, aml_u64(s));
    case 0x0D: s->pos++; return eval_string(s, out);
    case 0x11: s->pos++; return eval_buffer(scope, f, s, out);
    case 0x12: s->pos++; return eval_package(scope, f, s, false, out);
    case 0x13: s->pos++; return eval_package(scope, f, s, true, out);
    default: break;
    }
    if (b >= 0x60 && b <= 0x67) {
        s->pos++;
        return aml_value_copy(out, &f->local[b - 0x60], false);
    }
    if (b >= 0x68 && b <= 0x6E) {
        s->pos++;
        return aml_value_copy(out, &f->arg[b - 0x68], false);
    }
    if (name_lead(b)) return eval_name(scope, f, s, out);
    if (s->bad) return AML_ERR_BAD_CODE;

    if (b == 0x5B) {
        uint8_t second = (s->pos + 1 < s->end) ? s->base[s->pos + 1] : 0;
        s->pos += 2;
        return eval_extended(scope, f, s, (uint16_t)(0x5B00u | second), out);
    }

    uint8_t op = b;
    s->pos++;
    aml_status_t st2;
    switch (op) {
    case 0x70: {                                /* Store(source, target) */
        aml_value_t v;
        st2 = eval_arg(scope, f, s, &v);
        if (st2 != AML_OK) return st2;
        target_t t;
        st2 = parse_target(scope, f, s, &t);
        if (st2 != AML_OK) return st2;
        return finish_store(&t, &v, out);
    }
    case 0x71: {                                /* RefOf(name) */
        aml_path_t path;
        if (!aml_namestring(s, &path)) return AML_ERR_BAD_CODE;
        aml_node_t *n = aml_ns_lookup(scope, &path);
        if (n == NULL) return AML_ERR_NOT_FOUND;
        out->type = AML_T_REF;
        out->ref = n;
        return AML_OK;
    }
    case 0x72: case 0x74: case 0x77: case 0x79: case 0x7A:
    case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F: case 0x85:
        return binop(scope, f, s, op, out);
    case 0x78:
        return divide(scope, f, s, out);
    case 0x75: case 0x76: case 0x80: case 0x81: case 0x82: case 0x99:
        return unop(scope, f, s, op, out);
    case 0x83: {                                /* DerefOf(reference) */
        aml_value_t v;
        st2 = eval_arg(scope, f, s, &v);
        if (st2 != AML_OK) return st2;
        if (v.type == AML_T_REF) return aml_value_of_node(v.ref, out);
        *out = v;
        return AML_OK;
    }
    case 0x86: {                                /* Notify(object, value): recorded, not delivered yet */
        aml_path_t path;
        if (!aml_namestring(s, &path)) return AML_ERR_BAD_CODE;
        aml_value_t v;
        aml_status_t r = eval_arg(scope, f, s, &v);
        if (r == AML_OK) memset(out, 0, sizeof(*out));
        return r;
    }
    case 0x87: {                                /* SizeOf */
        aml_value_t v;
        st2 = eval_arg(scope, f, s, &v);
        if (st2 != AML_OK) return st2;
        if (v.type == AML_T_BUF) return set_int(out, v.len);
        if (v.type == AML_T_STR) return set_int(out, v.len ? v.len - 1 : 0);
        if (v.type == AML_T_PKG) return set_int(out, v.count);
        return AML_ERR_TYPE;
    }
    case 0x88: {                                /* Index(source, index, target) */
        aml_value_t src;
        st2 = eval_arg(scope, f, s, &src);
        if (st2 != AML_OK) return st2;
        uint64_t idx;
        st2 = eval_int(scope, f, s, &idx);
        if (st2 != AML_OK) return st2;
        target_t t;
        st2 = parse_target(scope, f, s, &t);
        if (st2 != AML_OK) return st2;
        if (t.kind != TG_NONE) return AML_ERR_UNSUPPORTED;   /* storing into an index reference */
        if (src.type == AML_T_PKG) {
            if (idx >= src.count) return AML_ERR_RANGE;
            return aml_value_copy(out, &src.elems[idx], false);
        }
        if (src.type == AML_T_BUF || src.type == AML_T_STR) {
            if (idx >= src.len) return AML_ERR_RANGE;
            return set_int(out, src.data[idx]);
        }
        return AML_ERR_TYPE;
    }
    case 0x8E: {                                /* ObjectType */
        aml_value_t v;
        st2 = eval_arg(scope, f, s, &v);
        if (st2 != AML_OK) return st2;
        switch (v.type) {
        case AML_T_INT: return set_int(out, 1);
        case AML_T_STR: return set_int(out, 2);
        case AML_T_BUF: return set_int(out, 3);
        case AML_T_PKG: return set_int(out, 4);
        case AML_T_REF:
            if (v.ref == NULL) return set_int(out, 0);
            if (v.ref->kind == NK_DEVICE) return set_int(out, 6);
            if (v.ref->kind == NK_METHOD) return set_int(out, 8);
            return set_int(out, 0);
        default: return set_int(out, 0);
        }
    }
    case 0x90: case 0x91: {                     /* LAnd, LOr */
        uint64_t x, y;
        st2 = eval_int(scope, f, s, &x);
        if (st2 != AML_OK) return st2;
        st2 = eval_int(scope, f, s, &y);
        if (st2 != AML_OK) return st2;
        return set_int(out, op == 0x90 ? (x && y) : (x || y));
    }
    case 0x92: {                                /* LNot */
        uint64_t x;
        st2 = eval_int(scope, f, s, &x);
        if (st2 != AML_OK) return st2;
        return set_int(out, !x);
    }
    case 0x93: case 0x94: case 0x95: {          /* LEqual, LGreater, LLess */
        aml_value_t a, b2;
        st2 = eval_arg(scope, f, s, &a);
        if (st2 != AML_OK) return st2;
        st2 = eval_arg(scope, f, s, &b2);
        if (st2 != AML_OK) return st2;
        int c = compare(&a, &b2, &st2);
        if (st2 != AML_OK) return st2;
        bool r = op == 0x93 ? c == 0 : op == 0x94 ? c > 0 : c < 0;
        return set_int(out, r);
    }
    case 0x9D: {                                /* CopyObject(source, destination) */
        aml_value_t v;
        st2 = eval_arg(scope, f, s, &v);
        if (st2 != AML_OK) return st2;
        target_t t;
        st2 = parse_target(scope, f, s, &t);
        if (st2 != AML_OK) return st2;
        return finish_store(&t, &v, out);
    }
    case 0x9F:                                  /* Continue used as a value: not valid */
    case 0xA0: case 0xA1: case 0xA2: case 0xA4: case 0xA5:
        return AML_ERR_BAD_CODE;
    case 0xA3: case 0xCC:                       /* Noop, Breakpoint */
        memset(out, 0, sizeof(*out));
        return AML_OK;
    case 0x73: case 0x84: case 0x89: case 0x8A: case 0x8B:
    case 0x8C: case 0x8D: case 0x8F: case 0x96: case 0x97:
    case 0x98: case 0x9C: case 0x9E:
        aml_log_unsupported(op, s->pos);
        return AML_ERR_UNSUPPORTED;
    default:
        aml_log_unsupported(op, s->pos);
        return AML_ERR_BAD_CODE;
    }
}

/* ---- statements ---- */

static aml_status_t exec_term(aml_node_t *scope, frame_t *f, aml_stream_t *s, uint32_t end, ctl_t *ctl) {
    uint8_t op = aml_peek(s);
    if (s->bad) return AML_ERR_BAD_CODE;
    switch (op) {
    case 0xA0: {                                /* If(predicate) { body } [Else { body }] */
        s->pos++;
        uint32_t if_end = aml_pkg_end(s);
        if (s->bad || if_end > end) return AML_ERR_BAD_CODE;
        uint64_t cond;
        aml_status_t st = eval_int(scope, f, s, &cond);
        if (st != AML_OK) return st;
        if (cond) {
            st = exec_block(scope, f, s, if_end, ctl);
            if (st != AML_OK) return st;
            s->pos = if_end;
            if (s->pos < end && aml_peek(s) == 0xA1) {
                s->pos++;
                uint32_t else_end = aml_pkg_end(s);
                if (s->bad || else_end > end) return AML_ERR_BAD_CODE;
                s->pos = else_end;
            }
        } else {
            s->pos = if_end;
            if (s->pos < end && aml_peek(s) == 0xA1) {
                s->pos++;
                uint32_t else_end = aml_pkg_end(s);
                if (s->bad || else_end > end) return AML_ERR_BAD_CODE;
                st = exec_block(scope, f, s, else_end, ctl);
                if (st != AML_OK) return st;
                s->pos = else_end;
            }
        }
        return AML_OK;
    }
    case 0xA1:                                  /* Else with no If */
        return AML_ERR_BAD_CODE;
    case 0xA2: {                                /* While(predicate) { body } */
        s->pos++;
        uint32_t wend = aml_pkg_end(s);
        if (s->bad || wend > end) return AML_ERR_BAD_CODE;
        uint32_t pred = s->pos;
        for (;;) {
            s->pos = pred;
            uint64_t cond;
            aml_status_t st = eval_int(scope, f, s, &cond);
            if (st != AML_OK) return st;
            if (!cond) break;
            ctl_t inner = CTL_NORMAL;
            st = exec_block(scope, f, s, wend, &inner);
            if (st != AML_OK) return st;
            if (inner == CTL_RETURN) {
                *ctl = CTL_RETURN;
                return AML_OK;
            }
            if (inner == CTL_BREAK) break;
        }
        s->pos = wend;
        return AML_OK;
    }
    case 0xA4: {                                /* Return(value) */
        s->pos++;
        aml_status_t st = eval_arg(scope, f, s, &f->ret);
        if (st != AML_OK) return st;
        f->has_ret = true;
        *ctl = CTL_RETURN;
        return AML_OK;
    }
    case 0xA5:                                  /* Break */
        s->pos++;
        *ctl = CTL_BREAK;
        return AML_OK;
    case 0x9F:                                  /* Continue */
        s->pos++;
        *ctl = CTL_CONTINUE;
        return AML_OK;
    default: {
        aml_value_t discard;
        return eval_arg(scope, f, s, &discard);
    }
    }
}

static aml_status_t exec_block(aml_node_t *scope, frame_t *f, aml_stream_t *s, uint32_t end, ctl_t *ctl) {
    while (s->pos < end) {
        aml_status_t st = spend();
        if (st != AML_OK) return st;
        st = exec_term(scope, f, s, end, ctl);
        if (st != AML_OK) return st;
        if (*ctl != CTL_NORMAL) return AML_OK;
        if (s->bad) return AML_ERR_BAD_CODE;
    }
    return s->pos == end ? AML_OK : AML_ERR_BAD_CODE;
}

/* A TermArg evaluated with no method frame, for the loader (Name values, region offsets). */
aml_status_t aml_eval_term_arg(aml_node_t *scope, aml_stream_t *s, aml_value_t *out) {
    frame_t fr;
    memset(&fr, 0, sizeof(fr));
    return eval_arg(scope, &fr, s, out);
}
