/* aml.h — the ACPI Machine Language (AML) interpreter: public interface.
 *
 * Firmware describes its devices and power methods as AML bytecode in the DSDT and SSDT
 * tables. This module loads that bytecode into a namespace (a tree of named objects, like
 * \_SB.PCI0) and can run methods such as _HID and _STA. Design and limits: docs/24-aml.md.
 *
 * Clean-room: the structure follows the ACPI 6.x specification and the way Linux's ACPICA
 * is organised, but the code is written for Helix. */
#ifndef HELIX_AML_H
#define HELIX_AML_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    AML_OK = 0,
    AML_ERR_NOT_LOADED,   /* no definition block has been loaded */
    AML_ERR_NOT_FOUND,    /* no object at that path */
    AML_ERR_BAD_CODE,     /* bytecode is malformed or runs past its package length */
    AML_ERR_UNSUPPORTED,  /* an opcode or feature Helix does not implement yet */
    AML_ERR_TYPE,         /* an operand has the wrong type for the operation */
    AML_ERR_RANGE,        /* index, bit position or divisor out of range */
    AML_ERR_DEPTH,        /* method calls nested too deeply */
    AML_ERR_BUDGET,       /* the step budget ran out (a loop that does not end) */
    AML_ERR_HARDWARE,     /* an OperationRegion could not be read or written */
    AML_ERR_FATAL,        /* the firmware called Fatal() */
} aml_status_t;

const char *aml_status_name(aml_status_t status);

typedef enum {
    AML_T_NONE = 0,
    AML_T_INT,    /* 64-bit integer (32-bit when the table revision is 1) */
    AML_T_STR,    /* NUL-terminated string; len counts the NUL */
    AML_T_BUF,    /* raw bytes */
    AML_T_PKG,    /* elems[0..count) */
    AML_T_REF,    /* a reference to a namespace node */
} aml_type_t;

typedef struct aml_node aml_node_t;

typedef struct aml_value {
    uint8_t type;
    uint64_t integer;
    uint8_t *data;           /* STR and BUF */
    uint32_t len;
    struct aml_value *elems; /* PKG */
    uint32_t count;
    aml_node_t *ref;         /* REF */
} aml_value_t;

/* Load every DSDT and SSDT that the firmware provided (see acpi.h). Safe to call once. */
aml_status_t aml_load_firmware(void);

/* Load one complete definition block (a whole table, header included). */
aml_status_t aml_load_block(const uint8_t *table, uint32_t length);

bool aml_loaded(void);
unsigned aml_node_count(void);

/* Evaluate a named object: a method (with nargs arguments) or a plain name. `path` is a
 * dotted ACPI path such as "\\_SB.PCI0._HID" (names shorter than four characters are padded
 * with '_'). On success the result is copied to the heap; release it with aml_value_free. */
aml_status_t aml_eval(const char *path, const aml_value_t *args, unsigned nargs, aml_value_t *out);

/* The node at a path (absolute, or relative to `scope` when scope is not NULL), or NULL. The
 * node pointer stays valid for the life of the system. */
aml_node_t *aml_lookup(aml_node_t *scope, const char *path);

/* The node's parent in the namespace tree, or NULL for the root. A device's parent is the
 * object its ACPI path sits under, for example the SPI controller that holds an SPI slave. */
aml_node_t *aml_parent(aml_node_t *node);

/* Copies a node's 4-character name into out and adds a NUL. */
void aml_node_name(const aml_node_t *node, char out[5]);

/* aml_eval, with the path resolved relative to `scope` (NULL means the root). */
aml_status_t aml_eval_at(aml_node_t *scope, const char *path, const aml_value_t *args, unsigned nargs,
                         aml_value_t *out);
void aml_value_free(aml_value_t *value);

/* Visit every named object. `path` is the absolute path, `kind` a short name ("Device",
 * "Method", "Name", "Region", "Field", ...), `argc` the argument count for methods. */
typedef void (*aml_walk_fn)(void *ctx, const char *path, const char *kind, unsigned argc);
void aml_walk(aml_walk_fn fn, void *ctx);

/* Read a device's _HID as text ("PNP0A08"). Returns false when it has none or it fails. */
bool aml_get_hid(const char *device_path, char out[9]);

#endif /* HELIX_AML_H */
