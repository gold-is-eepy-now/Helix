/* acpi_shim.c — the ACPI calls applespi makes. Helix parses the ACPI tables (kernel/src/acpi.c)
 * but has no AML interpreter, so it cannot run _GPE, UIST, SIEN or SIST, and it cannot read
 * device properties from the DSDT. Every call answers "not supported". A driver that gets
 * these answers must refuse to start; it must not pretend the hardware responded. */
#include <linux/acpi.h>
#include <linux/errno.h>

acpi_status acpi_evaluate_integer(acpi_handle handle, acpi_string pathname, struct acpi_object_list *args,
                                  unsigned long long *data) {
    (void)handle; (void)pathname; (void)args; (void)data;
    return AE_NOT_SUPPORTED;
}

acpi_status acpi_get_handle(acpi_handle parent, acpi_string pathname, acpi_handle *ret_handle) {
    (void)parent; (void)pathname;
    if (ret_handle) *ret_handle = NULL;
    return AE_NOT_SUPPORTED;
}

acpi_status acpi_execute_simple_method(acpi_handle handle, acpi_string method, u64 arg) {
    (void)handle; (void)method; (void)arg;
    return AE_NOT_SUPPORTED;
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
