/* linux/acpi.h — the ACPI calls a driver makes. Method evaluation and handle lookup go to the
 * AML interpreter (acpi_shim.c). GPE handlers and device properties (_DSD) are not implemented
 * and answer AE_NOT_SUPPORTED. */
#ifndef HELIX_COMPAT_LINUX_ACPI_H
#define HELIX_COMPAT_LINUX_ACPI_H
#include <linux/device.h>

typedef void *acpi_handle;
typedef u32 acpi_status;
typedef const char *acpi_string;
struct acpi_object_list;

#define AE_OK 0x0000u
#define AE_NOT_SUPPORTED 0x0003u
#define AE_NOT_FOUND 0x0005u
#define AE_ERROR 0x0001u
#define ACPI_SUCCESS(s) ((s) == AE_OK)
#define ACPI_FAILURE(s) ((s) != AE_OK)
#define ACPI_GPE_LEVEL_TRIGGERED 0x00u
#define ACPI_GPE_EDGE_TRIGGERED 0x01u
#define ACPI_INTERRUPT_HANDLED 0x01u
#define ACPI_TYPE_BUFFER 3u

union acpi_object {
    u32 type;
    struct { u32 type; u32 length; u8 *pointer; } buffer;
};

struct acpi_device_id { char id[16]; unsigned long driver_data; };

acpi_status acpi_evaluate_integer(acpi_handle handle, acpi_string pathname, struct acpi_object_list *args,
                                  unsigned long long *data);
acpi_status acpi_get_handle(acpi_handle parent, acpi_string pathname, acpi_handle *ret_handle);
acpi_status acpi_execute_simple_method(acpi_handle handle, acpi_string method, u64 arg);
const char *acpi_format_exception(acpi_status status);
acpi_status acpi_install_gpe_handler(acpi_handle gpe_device, u32 gpe, u32 type,
                                     u32 (*handler)(acpi_handle, u32, void *), void *context);
acpi_status acpi_remove_gpe_handler(acpi_handle gpe_device, u32 gpe, u32 (*handler)(acpi_handle, u32, void *));
acpi_status acpi_enable_gpe(acpi_handle gpe_device, u32 gpe);
acpi_status acpi_disable_gpe(acpi_handle gpe_device, u32 gpe);
acpi_status acpi_finish_gpe(acpi_handle gpe_device, u32 gpe);
int acpi_dev_get_property(const struct acpi_device *adev, const char *name, int type, const union acpi_object **obj);

#define ACPI_COMPANION(dev) ((dev)->acpi_companion)
/* The node the device was bound to by compat_acpi_find_hid(), or NULL if it was not bound. */
#define ACPI_HANDLE(dev) ((acpi_handle)(dev)->acpi_node)

/* Helix addition: the first Device in the AML namespace whose _HID is `hid`, or NULL. */
acpi_handle compat_acpi_find_hid(const char *hid);
#endif
