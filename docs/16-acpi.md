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

## What is not here

- **AML interpretation.** The device tree (which SPI controller, which GPIO pin
  raises the keyboard's interrupt) lives in the DSDT as bytecode. Reading it needs
  an AML interpreter. That is stage 2.
- **Power management and sleep tables** (FACS, DSDT methods for `_PS0`, and so on).

## Test

`make test` runs `acpi` in the shell suite and checks that the FADT and MADT are
listed with a valid checksum. QEMU's q35 firmware provides these tables.
