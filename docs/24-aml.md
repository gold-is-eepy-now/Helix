# 24. ACPI AML interpreter

Helix has its own interpreter for ACPI Machine Language (AML), the bytecode in the
DSDT and SSDT tables. It is written from the ACPI specification and checked against
the real firmware of the QEMU machine. Linux's ACPICA (BSD-3-Clause OR GPL-2.0) was
read for structure and edge cases; no ACPICA code is copied.

AML is how firmware describes devices. A device's name (`_HID`), its registers
(operation regions and fields), and its power and event methods (`_GPE`, `_STA`,
and so on) live in this bytecode, not in fixed tables. Without an interpreter, a
driver cannot ask the firmware about any of them.

## What works

- **Loading.** `aml_load_firmware()` finds the DSDT and every SSDT through the ACPI
  table code (docs/16-acpi.md) and loads each one into a single namespace tree. Loading
  checks each block's header length and records the table revision.
- **Namespace.** Scope, Device, Processor, PowerResource, ThermalZone, Method, Name,
  Alias, External, Mutex, Event, OperationRegion, Field, IndexField and BankField are
  all placed in the tree. A name used before its definition becomes a deferred reference
  and is resolved after the block loads.
- **Evaluation.** Methods run with up to 7 arguments and 8 local variables. Supported:
  integer, string and buffer arithmetic and logic, comparisons, `If`/`Else`, `While`,
  `Break`, `Continue`, `Return`, `Store`, `CopyObject`, `DerefOf`, `RefOf`, `SizeOf`,
  `ObjectType`, `ToInteger`, and `Index` when it is read from a package or buffer (writing through an
  `Index` reference is unsupported).
- **Fields over regions.** Reads and writes of fields in SystemMemory and SystemIO
  operation regions work. IndexField and BankField select their index and bank first.
- **Safety limits.** A recursion depth limit (8 nested method calls), a step budget
  (500 000 steps per top-level evaluation) so an endless `While` stops with an error,
  and a memory arena that is released after each top-level evaluation.

## Shell command

`aml` is in the command table (docs/02-shell.md):

- `aml` or `aml load`: load the firmware tables and print the object count.
- `aml tree`: list every namespace object with its kind.
- `aml hid`: list devices and their `_HID`. The PCI root decodes to `PNP0A08`
  (EISA ID `0x080AD041`).
- `aml eval <path>`: evaluate one object or method with no arguments.

## Tests

- `tests/aml-unit.c` (host build, run by `make test` after the boot tests): 14 checks on a
  hand-assembled table. They cover table rejection, a Name read, `_HID` decoding,
  method arithmetic, a `While` loop, a field read and write through a fake memory
  region, `If`/`Else`, `Index`, `Mod`, the step budget and an unknown path. The unit
  test is clean under AddressSanitizer and UndefinedBehaviorSanitizer.
- `tests/shell-test.py` boots QEMU and checks `aml hid` for `\_SB.PCI0  _HID PNP0A08`
  on the real DSDT.

## Not implemented

- **Device binding by `_HID` is done for one device (stage 1).** `compat_acpi_find_hid()`
  finds the AML Device with a given `_HID`, and the compat layer binds its node to the
  driver, so driver lookups start at that node. Binding by `_ADR` (PCI) or to a whole
  device tree is not implemented.
- **GPE1 and the control methods.** Only the GPE0 block is dispatched, to a driver's
  registered handler (docs/16-acpi.md). `_Lxx` and `_Exx` methods are not run when a GPE
  fires.
- **Other region spaces.** EmbeddedControl, PCI config, SMBus and the rest return
  "unsupported". Only SystemMemory and SystemIO work.
- **Access widths.** Field access width (`AccessAs`) is ignored; every access is
  byte-wide.
- **Notify.** `Notify` is parsed but not delivered to anything.
- **Opcodes.** The CreateField family, `Load`, `Concatenate`, `ToString`, `Timer`
  and `CondRefOf` are logged and stop evaluation with an error.
- **Sleep and Stall** busy-wait on the TSC. There is no sleeping yet.
- **Arguments from the shell.** `aml eval` takes no integer arguments yet.
- **Hardware.** Nothing has run on real hardware. The QEMU machine is the only
  firmware tested. This says nothing about the iMac 11,2 (which uses USB input).

## Related

- docs/16-acpi.md: the table parser this interpreter reads from.
- docs/08-linux-compat.md section 7: the applespi probe, which calls into this
  interpreter through the ACPI shim (`compat/src/acpi_shim.c`).
