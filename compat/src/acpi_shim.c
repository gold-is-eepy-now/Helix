/* acpi_shim.c — the ACPI calls applespi makes, answered by Helix's AML interpreter
 * (kernel/src/aml.c). An acpi_handle is an aml_node_t pointer; a NULL handle means the root.
 *
 * Implemented: finding a node by name, evaluating an object or method for an integer, calling
 * a method with one integer argument, and finding a device by _HID (compat_acpi_find_hid).
 * Not implemented: GPE handlers (no interrupt dispatch from ACPI yet) and device properties
 * (_DSD). Those still answer "not supported". */
#include <linux/acpi.h>
#include "acpi_gpe.h"
#include <linux/errno.h>
#include "aml.h"

static aml_node_t *scope_of(acpi_handle handle) {
    return (aml_node_t *)handle;
}

static acpi_status from_aml(aml_status_t st) {
    switch (st) {
    case AML_OK: return AE_OK;
    case AML_ERR_NOT_FOUND: return AE_NOT_FOUND;
    case AML_ERR_NOT_LOADED:
    case AML_ERR_UNSUPPORTED: return AE_NOT_SUPPORTED;
    default: return AE_ERROR;
    }
}

acpi_status acpi_evaluate_integer(acpi_handle handle, acpi_string pathname, struct acpi_object_list *args,
                                  unsigned long long *data) {
    (void)args;
    aml_value_t v;
    /* ACPICA: a NULL pathname means "the object the handle names". The path parser cannot take
     * NULL, so the empty path is used: it resolves to the scope itself. */
    aml_status_t st = aml_eval_at(scope_of(handle), pathname ? pathname : "", NULL, 0, &v);
    if (st != AML_OK) return from_aml(st);
    uint64_t x = 0;
    st = (v.type == AML_T_INT) ? AML_OK : AML_ERR_TYPE;
    if (st == AML_OK) x = v.integer;
    aml_value_free(&v);
    if (st != AML_OK) return AE_ERROR;
    if (data) *data = x;
    return AE_OK;
}

acpi_status acpi_get_handle(acpi_handle parent, acpi_string pathname, acpi_handle *ret_handle) {
    aml_node_t *n = aml_lookup(scope_of(parent), pathname);
    if (n == NULL) return AE_NOT_FOUND;
    if (ret_handle) *ret_handle = (acpi_handle)n;
    return AE_OK;
}

/* Walks the namespace once to collect Device paths, then reads each one's _HID. The walk
 * only records paths; _HID is evaluated afterwards, so the tree is not read while visited. */
#define HID_SCAN_MAX 64
#define HID_PATH_MAX 96
struct hid_scan {
    char paths[HID_SCAN_MAX][HID_PATH_MAX];
    unsigned count;
};

static void collect_device(void *ctx, const char *path, const char *kind, unsigned argc) {
    struct hid_scan *s = (struct hid_scan *)ctx;
    (void)argc;
    if (strcmp(kind, "Device") != 0 || s->count >= HID_SCAN_MAX) return;
    size_t n = strlen(path);
    if (n >= HID_PATH_MAX) return;
    memcpy(s->paths[s->count], path, n + 1);
    s->count++;
}

acpi_handle compat_acpi_find_hid(const char *hid) {
    if (hid == NULL || !aml_loaded()) return NULL;
    static struct hid_scan scan;
    scan.count = 0;
    aml_walk(collect_device, &scan);
    for (unsigned i = 0; i < scan.count; i++) {
        char got[9];
        if (aml_get_hid(scan.paths[i], got) && strcmp(got, hid) == 0)
            return (acpi_handle)aml_lookup(NULL, scan.paths[i]);
    }
    return NULL;
}

acpi_status acpi_execute_simple_method(acpi_handle handle, acpi_string method, u64 arg) {
    aml_value_t a;
    a.type = AML_T_INT;
    a.integer = arg;
    a.data = NULL;
    a.len = 0;
    a.elems = NULL;
    a.count = 0;
    a.ref = NULL;
    aml_value_t out;
    aml_status_t st = aml_eval_at(scope_of(handle), method ? method : "", &a, 1, &out);
    if (st == AML_OK) aml_value_free(&out);
    return from_aml(st);
}

/* The names ACPICA uses for the codes this shim returns. Anything else is shown as a number,
 * so a failure is never reported with the wrong reason. */
const char *acpi_format_exception(acpi_status status) {
    switch (status) {
    case AE_OK: return "AE_OK";
    case AE_ERROR: return "AE_ERROR";
    case AE_NOT_FOUND: return "AE_NOT_FOUND";
    case AE_NOT_SUPPORTED: return "AE_NOT_SUPPORTED";
    case AE_ALREADY_EXISTS: return "AE_ALREADY_EXISTS";
    case AE_BAD_PARAMETER: return "AE_BAD_PARAMETER";
    default: return "AE_UNKNOWN";
    }
}

/* Maps the kernel's GPE result codes to the status codes applespi expects. */
static acpi_status gpe_status(uint32_t r) {
    switch (r) {
    case GPE_OK: return AE_OK;
    case GPE_ERR_EXISTS: return AE_ALREADY_EXISTS;
    case GPE_ERR_NO_HANDLER: return AE_NOT_FOUND;
    case GPE_ERR_RANGE: return AE_BAD_PARAMETER;
    default: return AE_NOT_SUPPORTED;   /* not ready: no usable FADT blocks, or not in ACPI mode */
    }
}

/* ACPICA passes NULL for the system's own GPE block, which is the only one Helix has (GPE0).
 * A named GPE block device is refused. */
acpi_status acpi_install_gpe_handler(acpi_handle gpe_device, u32 gpe, u32 type,
                                     u32 (*handler)(acpi_handle, u32, void *), void *context) {
    if (gpe_device != NULL) return AE_NOT_SUPPORTED;
    return gpe_status(acpi_gpe_install(gpe, type == ACPI_GPE_LEVEL_TRIGGERED,
                                       (acpi_gpe_handler_t)handler, NULL, context));
}

acpi_status acpi_remove_gpe_handler(acpi_handle gpe_device, u32 gpe, u32 (*handler)(acpi_handle, u32, void *)) {
    if (gpe_device != NULL) return AE_NOT_SUPPORTED;
    return gpe_status(acpi_gpe_remove(gpe, (acpi_gpe_handler_t)handler));
}

acpi_status acpi_enable_gpe(acpi_handle gpe_device, u32 gpe) {
    if (gpe_device != NULL) return AE_NOT_SUPPORTED;
    return gpe_status(acpi_gpe_enable(gpe, true));
}

acpi_status acpi_disable_gpe(acpi_handle gpe_device, u32 gpe) {
    if (gpe_device != NULL) return AE_NOT_SUPPORTED;
    return gpe_status(acpi_gpe_enable(gpe, false));
}

acpi_status acpi_finish_gpe(acpi_handle gpe_device, u32 gpe) {
    if (gpe_device != NULL) return AE_NOT_SUPPORTED;
    return gpe_status(acpi_gpe_finish(gpe));
}

/* Non-zero means "not found", as in Linux. */
int acpi_dev_get_property(const struct acpi_device *adev, const char *name, int type, const union acpi_object **obj) {
    (void)adev; (void)name; (void)type;
    if (obj) *obj = NULL;
    return -ENODEV;
}
