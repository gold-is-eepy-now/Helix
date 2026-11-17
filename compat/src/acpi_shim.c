/* acpi_shim.c — the ACPI calls applespi makes, answered by Helix's AML interpreter
 * (kernel/src/aml.c). An acpi_handle is an aml_node_t pointer; a NULL handle means the root.
 *
 * Implemented: finding a node by name, evaluating an object or method for an integer, and
 * calling a method with one integer argument. Not implemented: GPE handlers (no interrupt
 * dispatch from ACPI yet) and device properties (_DSD). Those still answer "not supported". */
#include <linux/acpi.h>
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
    aml_status_t st = aml_eval_at(scope_of(handle), pathname, NULL, 0, &v);
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
    aml_status_t st = aml_eval_at(scope_of(handle), method, &a, 1, &out);
    if (st == AML_OK) aml_value_free(&out);
    return from_aml(st);
}

const char *acpi_format_exception(acpi_status status) {
    (void)status;
    return "AE_NOT_SUPPORTED (Helix has no AML interpreter)";
}

acpi_status acpi_install_gpe_handler(acpi_handle gpe_device, u32 gpe, u32 type,
                                     u32 (*handler)(acpi_handle, u32, void *), void *context) {
    (void)gpe_device; (void)gpe; (void)type; (void)handler; (void)context;
    return AE_NOT_SUPPORTED;
}

acpi_status acpi_remove_gpe_handler(acpi_handle gpe_device, u32 gpe, u32 (*handler)(acpi_handle, u32, void *)) {
    (void)gpe_device; (void)gpe; (void)handler;
    return AE_NOT_SUPPORTED;
}

acpi_status acpi_enable_gpe(acpi_handle gpe_device, u32 gpe) {
    (void)gpe_device; (void)gpe;
    return AE_NOT_SUPPORTED;
}

acpi_status acpi_disable_gpe(acpi_handle gpe_device, u32 gpe) {
    (void)gpe_device; (void)gpe;
    return AE_NOT_SUPPORTED;
}

acpi_status acpi_finish_gpe(acpi_handle gpe_device, u32 gpe) {
    (void)gpe_device; (void)gpe;
    return AE_NOT_SUPPORTED;
}

/* Non-zero means "not found", as in Linux. */
int acpi_dev_get_property(const struct acpi_device *adev, const char *name, int type, const union acpi_object **obj) {
    (void)adev; (void)name; (void)type;
    if (obj) *obj = NULL;
    return -ENODEV;
}
