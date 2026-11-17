/* aml_ns.c — the namespace: a tree of named objects rooted at \.
 *
 * Each node has a four-character name, a kind, a parent, and a list of children. Lookup
 * follows ACPI's rules: a name with no root or parent prefix is searched for in the current
 * scope, then in each enclosing scope; a path with several segments is searched from the
 * first segment that was found. */
#include "aml_internal.h"
#include "heap.h"
#include "kprintf_compat.h"
#include "kstring.h"

uint8_t aml_revision = 2;
uint32_t aml_budget = 0;
unsigned aml_depth = 0;
bool aml_loading = false;

static aml_node_t *g_root;
static unsigned g_nodes;

aml_node_t *aml_ns_root(void) {
    if (g_root == NULL) {
        g_root = kmalloc(sizeof(aml_node_t));
        if (g_root) {
            memset(g_root, 0, sizeof(*g_root));
            memcpy(g_root->name, "\\___", 4);
            g_root->kind = NK_ROOT;
        }
    }
    return g_root;
}

void aml_ns_reset(void) {
    /* Nodes are kept for the life of the system: a loaded namespace is never unloaded. */
}

aml_node_t *aml_ns_child(aml_node_t *parent, const char name[4]) {
    for (aml_node_t *n = parent->child; n; n = n->next) {
        if (memcmp(n->name, name, 4) == 0) return n;
    }
    return NULL;
}

aml_node_t *aml_ns_new(aml_node_t *parent, const char name[4], uint8_t kind) {
    aml_node_t *n = kmalloc(sizeof(aml_node_t));
    if (n == NULL) return NULL;
    memset(n, 0, sizeof(*n));
    memcpy(n->name, name, 4);
    n->kind = kind;
    n->parent = parent;
    n->next = parent->child;     /* newest first; lookups are short lists */
    parent->child = n;
    g_nodes++;
    return n;
}

unsigned aml_node_count(void) { return g_nodes; }

/* Find the first node for one NameSeg by walking up from `scope`. */
static aml_node_t *search_up(aml_node_t *scope, const char name[4]) {
    for (aml_node_t *s = scope; s; s = s->parent) {
        aml_node_t *n = aml_ns_child(s, name);
        if (n) return n;
    }
    return NULL;
}

aml_node_t *aml_ns_lookup(aml_node_t *scope, const aml_path_t *path) {
    aml_node_t *node;
    unsigned seg = 0;
    if (path->root) {
        node = aml_ns_root();
    } else if (path->parents) {
        node = scope;
        for (unsigned i = 0; i < path->parents && node; i++) node = node->parent;
    } else {
        if (path->nseg == 0) return scope;
        node = search_up(scope, path->seg[0]);
        if (node == NULL) return NULL;
        seg = 1;
    }
    for (; seg < path->nseg && node; seg++) {
        node = aml_ns_child(node, path->seg[seg]);
    }
    return node;
}

/* Define a named object. A Scope may name an existing node; any other kind must be new. The
 * parent is the node named by all but the last segment (which must already exist), and the
 * last segment is created under it. */
aml_status_t aml_ns_define(aml_node_t *scope, const aml_path_t *path, uint8_t kind, aml_node_t **out) {
    if (path->nseg == 0) return AML_ERR_BAD_CODE;
    aml_path_t prefix = *path;
    prefix.nseg = (uint8_t)(path->nseg - 1);
    aml_node_t *parent = aml_ns_lookup(scope, &prefix);
    if (parent == NULL) {
        kprintf("[aml] parent missing for ");
        for (unsigned i = 0; i < path->parents; i++) kprintf("^");
        for (unsigned i = 0; i < path->nseg; i++) {
            for (unsigned c = 0; c < 4; c++) kprintf("%c", path->seg[i][c]);
            kprintf(i + 1 < path->nseg ? "." : "");
        }
        kprintf("\n");
        return AML_ERR_NOT_FOUND;
    }
    const char *name = path->seg[path->nseg - 1];
    aml_node_t *existing = aml_ns_child(parent, name);
    if (existing) {
        if (kind == NK_SCOPE && existing->kind != NK_METHOD) {
            *out = existing;
            return AML_OK;
        }
        return AML_ERR_BAD_CODE;          /* a name may be defined only once */
    }
    aml_node_t *n = aml_ns_new(parent, name, kind);
    if (n == NULL) return AML_ERR_NOT_LOADED;
    *out = n;
    return AML_OK;
}

void aml_ns_path_text(const aml_node_t *node, char *buf, unsigned size) {
    /* Collect the chain of ancestors, then write them from the root down. Trailing '_' padding
     * is dropped ("_SB_" is shown as "_SB"), but the first character is always kept. */
    const aml_node_t *chain[32];
    unsigned depth = 0;
    for (const aml_node_t *n = node; n && n->kind != NK_ROOT && depth < 32; n = n->parent) {
        chain[depth++] = n;
    }
    if (size < 2) return;
    unsigned at = 0;
    buf[at++] = '\\';
    for (unsigned i = depth; i-- > 0;) {
        if (i + 1 != depth && at + 1 < size) buf[at++] = '.';
        unsigned len = 4;
        while (len > 1 && chain[i]->name[len - 1] == '_') len--;
        for (unsigned c = 0; c < len && at + 1 < size; c++) buf[at++] = chain[i]->name[c];
    }
    buf[at] = '\0';
}

/* Parse "\_SB.PCI0._HID" style text. Names shorter than four characters are padded with '_'
 * so "_SB" and "_SB_" name the same object. */
bool aml_path_from_text(const char *text, aml_path_t *out) {
    for (unsigned i = 0; i < sizeof(*out); i++) ((uint8_t *)out)[i] = 0;
    if (*text == '\\') {
        out->root = true;
        text++;
    }
    while (*text == '^') {
        out->parents++;
        text++;
    }
    while (*text) {
        if (out->nseg >= 16) return false;
        char *seg = out->seg[out->nseg];
        unsigned n = 0;
        while (*text && *text != '.') {
            if (n >= 4) return false;
            seg[n++] = *text++;
        }
        if (n == 0) return false;
        while (n < 4) seg[n++] = '_';
        out->nseg++;
        if (*text == '.') text++;
    }
    return true;
}
