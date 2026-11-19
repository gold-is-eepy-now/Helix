/* acpi_gpe.h — ACPI general-purpose events and the System Control Interrupt (SCI).
 *
 * The firmware wires the SCI to one PIC line (FADT SCI_INT). When an enabled event fires, its
 * status bit is set and the SCI is raised. Helix's handler reads the PM1 and GPE0 status,
 * clears the bits that fired, and calls the handler a driver installed for that GPE. The PM1
 * power button is handled here too: a press is logged.
 *
 * Level-triggered GPEs stay disabled after they fire until the driver calls
 * acpi_gpe_finish(), as ACPICA requires.
 *
 * Not done: GPE1, the _Lxx/_Exx control methods (AML that runs when a GPE fires), wake
 * events, and PM1 events other than the power button. See docs/16-acpi.md. */
#ifndef HELIX_ACPI_GPE_H
#define HELIX_ACPI_GPE_H

#include <stdbool.h>
#include <stdint.h>

#define ACPI_GPE_SLOTS 128u

enum {
    GPE_OK = 0,
    GPE_ERR_NOT_READY,     /* no usable FADT blocks, or the firmware is not in ACPI mode */
    GPE_ERR_EXISTS,        /* a handler is already installed for this GPE */
    GPE_ERR_RANGE,         /* the GPE is not in block 0 */
    GPE_ERR_NO_HANDLER,    /* nothing installed for this GPE, or not this handler */
};

/* Called with the GPE number and the context given at install. */
typedef uint32_t (*acpi_gpe_handler_t)(void *device, uint32_t gpe, void *ctx);

/* Reads the FADT blocks, quiets the hardware, enables the power button, and hooks the SCI
 * into the IRQ table. Returns false (and says why) when the platform cannot deliver events. */
bool acpi_gpe_init(void);
bool acpi_gpe_ready(void);

uint32_t acpi_gpe_install(uint32_t gpe, bool level, acpi_gpe_handler_t fn, void *device, void *ctx);
uint32_t acpi_gpe_remove(uint32_t gpe, acpi_gpe_handler_t fn);
uint32_t acpi_gpe_enable(uint32_t gpe, bool on);
uint32_t acpi_gpe_finish(uint32_t gpe);

#endif /* HELIX_ACPI_GPE_H */
