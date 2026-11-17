/* linux/efi.h — EFI runtime variables. Helix keeps no EFI runtime services table, so the
 * runtime-support query answers false and the variable calls are never reached. */
#ifndef HELIX_COMPAT_LINUX_EFI_H
#define HELIX_COMPAT_LINUX_EFI_H
#include <linux/types.h>
typedef unsigned long efi_status_t;
typedef __WCHAR_TYPE__ efi_char16_t;    /* wchar_t here; the stub never reads the name */
typedef struct { u8 b[16]; } efi_guid_t;
#define EFI_SUCCESS 0ul
#define EFI_UNSUPPORTED 3ul
#define EFI_NOT_FOUND 14ul
#define EFI_VARIABLE_NON_VOLATILE 0x1u
#define EFI_VARIABLE_BOOTSERVICE_ACCESS 0x2u
#define EFI_VARIABLE_RUNTIME_ACCESS 0x4u
#define EFI_RT_SUPPORTED_GET_VARIABLE 0x1u
#define EFI_RT_SUPPORTED_SET_VARIABLE 0x4u
#define EFI_GUID(a, b, c, d0, d1, d2, d3, d4, d5, d6, d7) \
    ((efi_guid_t){ { (a) & 0xff, ((a) >> 8) & 0xff, ((a) >> 16) & 0xff, ((a) >> 24) & 0xff, \
        (b) & 0xff, ((b) >> 8) & 0xff, (c) & 0xff, ((c) >> 8) & 0xff, d0, d1, d2, d3, d4, d5, d6, d7 } })
struct efi_runtime {
    efi_status_t (*get_variable)(efi_char16_t *name, efi_guid_t *vendor, u32 *attr, unsigned long *size, void *data);
    efi_status_t (*set_variable)(efi_char16_t *name, efi_guid_t *vendor, u32 attr, unsigned long size, void *data);
};
extern struct efi_runtime efi;
bool efi_rt_services_supported(unsigned int mask);
#endif
