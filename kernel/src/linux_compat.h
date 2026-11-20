/* linux_compat.h — entry points of the Linux compatibility layer (kernel side).
 * See docs/08-linux-compat.md. The headers drivers include live in compat/include. */
#ifndef HELIX_LINUX_COMPAT_H
#define HELIX_LINUX_COMPAT_H

/* Run every module_init function collected into the compat_initcalls section.
 * Returns the number that failed (0 means all succeeded). */
int compat_run_initcalls(void);

/* Checks for the `inputtest` shell command: the Linux input shim and the applespi probe
 * path (compat/src/input_selftest.c). Prints one line per check; returns failures. */
int compat_input_selftest(void);

/* Prints how an ACPI SPI slave (by _HID, e.g. "APP000D") is linked to a controller: its parent,
 * its _CRS settings and whether the link succeeded, and why not. Sends nothing (spi command). */
void compat_spi_print_link(const char *acpi_hid);

#endif /* HELIX_LINUX_COMPAT_H */
