# applespi (Linux) — reference copy, GPL-2.0

`applespi.c` and `applespi.h` are copied unchanged from the Linux kernel
(`drivers/input/keyboard/`, fetched from github.com/torvalds/linux, master, on 2026-11-16).
They are **GPL-2.0-only**, copyright Federico Lorenzi and Ronald Tschalär.

This folder exists only on the `super-duper-experimental` branch, like `experiments/gnumach`.
**Nothing here is part of the Helix kernel, nothing is built from it, and it must never be
merged into `desktop-test` or any branch that ships under Helix's BSD-3-Clause licence.**
Copying code or structure from it into `kernel/` would put GPL code in Helix.

## Status

Not ported. It is written against Linux's SPI, ACPI, GPIO, input and crypto-free driver
interfaces (`spi_device`, `acpi_*`, `input_dev`, `gpiod_*`, workqueues, `devm_*`), none of
which exist in Helix, so it will not compile here. Helix's own clean-room probe is
`kernel/src/apple_topcase_spi.c` (docs/18-topcase.md).

## One fact from the file's header comment worth knowing

The header says the SPI path is needed on MacBookAir6/7-class machines only when the USB
pins are not connected, and that the interface is chosen with ACPI methods (`UIEN`/`SIEN`).
It says the USB pins *are* connected on the MacBookAir6 and 7 and the MacBookPro12. If that is
right for the Air 7,2, its keyboard might be reachable over USB instead of SPI, but switching
interfaces needs an AML interpreter, which Helix does not have yet. This is the file's claim; it
has not been checked on hardware.
