# 16. ACPI tables

Status: **stage 1 of 5** for the Apple SPI keyboard and trackpad (MacBook Air 7,2).
QEMU-tested only. Not yet tested on real hardware.

## What is here

- `kernel/src/acpi.c` finds the RSDP through the Limine boot request, checks its
  checksum, and walks the root table (XSDT on ACPI 2.0 and later, RSDT on 1.0).
- Every table's signature, length and checksum is recorded (up to 32 tables).
- The DSDT is recorded too, found through the FADT, even though it is not in the
  root table. The DSDT holds the device tree in ACPI bytecode (AML).
- The `acpi` shell command lists them.
- The `pci` shell command lists every PCI function with its vendor and device IDs,
  class and BAR0. Run it on the MacBook Air to find the GSPI controller's real IDs
  before the SPI driver matches on them. We do not hard-code IDs from memory.

## ACPI events: the SCI and the GPE0 block (QEMU-tested only)

- `acpi_get_pm()` reads the FADT's fixed hardware blocks as I/O ports: the SCI line
  (`SCI_INT`), the PM1a event and control blocks, and the GPE0 block. The 64-bit generic
  address is used when the FADT is long enough; otherwise the 32-bit field.
- `acpi_gpe_init()` (called at boot, after the AML namespace loads) refuses unless `SCI_EN`
  is set, meaning the firmware is in ACPI mode. It then clears pending status, disables
  every GPE, enables only the PM1 power button, marks the SCI line level-triggered in the
  PIC's ELCR, and registers its handler in the IRQ table (`kernel/src/irq.c`). The SCI on
  QEMU's q35 is IRQ 9, with PM1a at 0x600 and GPE0 at 0x620 (64 GPEs).
- When the SCI fires, the handler reads PM1 and GPE0 status masked by enable. It clears
  only the bits that fired, logs a power button press, and calls the handler a driver
  installed for each GPE. A level GPE is disabled until the driver calls
  `acpi_gpe_finish()`, as ACPICA requires. A GPE with no handler is disabled.
- The compat layer (`compat/src/acpi_shim.c`) maps `acpi_install_gpe_handler`,
  `acpi_enable_gpe`, `acpi_disable_gpe`, `acpi_finish_gpe` and `acpi_remove_gpe_handler`
  onto these functions. Only the default GPE block is accepted; a named block device is
  refused.
- The power button test is real: the suite sends `system_powerdown` through QEMU's monitor,
  and the SCI handler logs `[acpi] power button pressed`. This tests the PIC line and the
  handler end to end, but not any GPE from a device.

Not yet: GPE1, `_Lxx`/`_Exx` methods (AML that runs when a GPE fires), PM1 events other
than the power button, wake events, and the SCI on the MacBook Air, which is untested.

## What is not here

- **Power management and sleep tables** (FACS, DSDT methods for `_PS0`, and so on).
- The GPE status and enable registers have not been checked against a real Apple
  firmware. The register layout follows ACPI 6.

## Test

`make test` runs `acpi` in the shell suite and checks that the FADT and MADT are
listed with a valid checksum. QEMU's q35 firmware provides these tables. The suite also
checks that the boot log reports the SCI line, and that a power button press sent through
the monitor reaches the SCI handler.
