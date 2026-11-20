/* aml.c — the AML public API: load the firmware's tables, evaluate objects by path, walk the
 * namespace, and read device IDs. See aml.h and docs/24-aml.md. */
#include "aml_internal.h"
#include "acpi.h"
#include "kprintf_compat.h"
#include "kstring.h"

#define HEADER_LEN 36u
#define STEP_BUDGET 500000u

static bool g_loaded;
static bool g_revision_set;

const char *aml_status_name(aml_status_t status) {
    switch (status) {
    case AML_OK: return "ok";
    case AML_ERR_NOT_LOADED: return "no tables loaded";
    case AML_ERR_NOT_FOUND: return "not found";
    case AML_ERR_BAD_CODE: return "malformed bytecode";
    case AML_ERR_UNSUPPORTED: return "not supported yet";
    case AML_ERR_TYPE: return "wrong type";
    case AML_ERR_RANGE: return "out of range";
    case AML_ERR_DEPTH: return "methods nested too deeply";
    case AML_ERR_BUDGET: return "step budget exhausted";
    case AML_ERR_HARDWARE: return "region access failed";
    case AML_ERR_FATAL: return "firmware called Fatal()";
    }
    return "unknown";
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

aml_status_t aml_load_block(const uint8_t *table, uint32_t length) {
    if (table == NULL || length < HEADER_LEN) return AML_ERR_BAD_CODE;
    uint32_t declared = le32(table + 4);
    if (declared < HEADER_LEN || declared > length) return AML_ERR_BAD_CODE;
    /* The first block's revision decides integer width for the whole namespace (ACPI says
     * each block has its own; Helix uses the DSDT's, which is the one that matters here). */
    if (!g_revision_set) {
        aml_revision = table[8];
        g_revision_set = true;
    }
    aml_stream_t s = { table, HEADER_LEN, declared, false };
    aml_budget = STEP_BUDGET;
    aml_depth = 0;
    aml_loading = true;
    aml_status_t st = aml_load_terms(aml_ns_root(), &s);
    aml_loading = false;
    aml_resolve_references();
    if (st == AML_OK) g_loaded = true;
    return st;
}

static aml_status_t load_one(const uint8_t *table) {
    uint32_t len = le32(table + 4);
    return aml_load_block(table, len);
}

aml_status_t aml_load_firmware(void) {
    if (g_loaded) return AML_OK;
    const void *dsdt = acpi_find("DSDT");
    if (dsdt == NULL) return AML_ERR_NOT_FOUND;
    aml_status_t st = load_one((const uint8_t *)dsdt);
    if (st != AML_OK) {
        kprintf("[aml] DSDT did not load: %s\n", aml_status_name(st));
        return st;
    }
    unsigned loaded = 1;
    for (unsigned i = 0; i < acpi_table_count(); i++) {
        acpi_table_t t;
        if (!acpi_table_at(i, &t)) continue;
        if (strcmp(t.sig, "SSDT") != 0) continue;
        if (!t.checksum_ok) {
            kprintf("[aml] SSDT %u skipped: bad checksum\n", i);
            continue;
        }
        const uint8_t *p = acpi_table_data(i);
        if (p == NULL) continue;
        aml_status_t s2 = load_one(p);
        if (s2 == AML_OK) {
            loaded++;
        } else {
            kprintf("[aml] SSDT %u did not load: %s\n", i, aml_status_name(s2));
        }
    }
    kprintf("[aml] namespace: %u tables, %u objects\n", loaded, aml_node_count());
    return AML_OK;
}

bool aml_loaded(void) { return g_loaded; }

void aml_value_free(aml_value_t *value) {
    aml_value_destroy(value);
}

aml_node_t *aml_parent(aml_node_t *node) {
    return node != NULL ? node->parent : NULL;
}

void aml_node_name(const aml_node_t *node, char out[5]) {
    for (int i = 0; i < 4; i++) out[i] = node != NULL ? node->name[i] : '?';
    out[4] = '\0';
}

aml_node_t *aml_lookup(aml_node_t *scope, const char *path) {
    aml_path_t p;
    if (!aml_path_from_text(path, &p)) return NULL;
    aml_node_t *n = aml_ns_lookup(scope ? scope : aml_ns_root(), &p);
    while (n && n->kind == NK_ALIAS) n = n->u.alias.target;
    return n;
}

aml_status_t aml_eval(const char *path, const aml_value_t *args, unsigned nargs, aml_value_t *out) {
    return aml_eval_at(NULL, path, args, nargs, out);
}

/* Path text, then the object. Arguments are copied into the arena for the call. */
aml_status_t aml_eval_at(aml_node_t *scope, const char *path, const aml_value_t *args, unsigned nargs,
                         aml_value_t *out) {
    if (!g_loaded) return AML_ERR_NOT_LOADED;
    aml_node_t *n = aml_lookup(scope, path);
    if (n == NULL) return AML_ERR_NOT_FOUND;

    void *mark = aml_arena_mark();
    aml_budget = STEP_BUDGET;
    aml_depth = 0;
    aml_value_t result;
    memset(&result, 0, sizeof(result));
    aml_status_t st;
    if (n->kind == NK_METHOD) {
        aml_value_t copies[7];
        unsigned argc = nargs < 7 ? nargs : 7;
        for (unsigned i = 0; i < argc; i++) {
            st = aml_value_copy(&copies[i], &args[i], false);
            if (st != AML_OK) goto done;
        }
        st = aml_invoke(n, copies, argc, &result);
    } else {
        st = aml_value_of_node(n, &result);
    }
done:
    if (st == AML_OK) {
        st = aml_value_copy(out, &result, true);
    }
    aml_arena_release(mark);
    return st;
}

typedef struct { aml_walk_fn fn; void *ctx; } walk_ctx_t;

static const char *kind_name(uint8_t kind) {
    switch (kind) {
    case NK_ROOT: return "Root";
    case NK_SCOPE: return "Scope";
    case NK_DEVICE: return "Device";
    case NK_PROCESSOR: return "Processor";
    case NK_POWER: return "PowerResource";
    case NK_THERMAL: return "ThermalZone";
    case NK_METHOD: return "Method";
    case NK_NAME: return "Name";
    case NK_REGION: return "Region";
    case NK_FIELD: return "Field";
    case NK_MUTEX: return "Mutex";
    case NK_EVENT: return "Event";
    case NK_ALIAS: return "Alias";
    }
    return "?";
}

static void walk_node(aml_node_t *n, walk_ctx_t *w) {
    char path[96];
    if (n->kind != NK_ROOT) {
        aml_ns_path_text(n, path, sizeof(path));
        w->fn(w->ctx, path, kind_name(n->kind), n->kind == NK_METHOD ? n->u.method.argc : 0);
    }
    for (aml_node_t *c = n->child; c; c = c->next) walk_node(c, w);
}

void aml_walk(aml_walk_fn fn, void *ctx) {
    walk_ctx_t w = { fn, ctx };
    walk_node(aml_ns_root(), &w);
}

/* EISA ID: an integer whose low word holds three 5-bit letters (big-endian), then four hex
 * digits of product code. PNP0A08 is the integer 0x080AD041. */
static void eisa_text(uint32_t id, char out[9]) {
    uint16_t word = (uint16_t)(((id & 0xFFu) << 8) | ((id >> 8) & 0xFFu));
    out[0] = (char)(((word >> 10) & 0x1F) + 0x40);
    out[1] = (char)(((word >> 5) & 0x1F) + 0x40);
    out[2] = (char)((word & 0x1F) + 0x40);
    static const char hex[] = "0123456789ABCDEF";
    /* The product code's bytes are stored in reverse order: 0x0A then 0x08 read as "0A08". */
    unsigned product = ((((id >> 16) & 0xFFu) << 8) | ((id >> 24) & 0xFFu));
    out[3] = hex[(product >> 12) & 0xF];
    out[4] = hex[(product >> 8) & 0xF];
    out[5] = hex[(product >> 4) & 0xF];
    out[6] = hex[product & 0xF];
    out[7] = '\0';
    out[8] = '\0';
}

bool aml_get_hid(const char *device_path, char out[9]) {
    char path[96];
    unsigned n = 0;
    for (const char *p = device_path; *p && n + 8 < sizeof(path); p++) path[n++] = *p;
    const char *suffix = "._HID";
    for (const char *p = suffix; *p; p++) path[n++] = *p;
    path[n] = '\0';

    aml_value_t v;
    if (aml_eval(path, NULL, 0, &v) != AML_OK) return false;
    bool ok = false;
    if (v.type == AML_T_INT) {
        eisa_text((uint32_t)v.integer, out);
        ok = true;
    } else if (v.type == AML_T_STR && v.len) {
        unsigned i = 0;
        for (; i < 8 && i + 1 < v.len; i++) out[i] = (char)v.data[i];
        out[i] = '\0';
        ok = true;
    }
    aml_value_free(&v);
    return ok;
}
