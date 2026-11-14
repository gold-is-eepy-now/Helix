# Linux on Helix's boot path (experiment)

Branch: `super-duper-experimental`. One-off experiment.

## What it shows

A custom-built **Linux 7.2** kernel boots from a UEFI ISO through **Limine**, the
same bootloader Helix uses, and runs a static `/init` as PID 1. The banner comes out
over the serial port. See `evidence/serial-boot.log`.

Helix's own kernel is **not** in this ISO. Limine loads Linux instead of Helix, so this
shows the boot path and a Linux kernel running. It does not run Helix code.

## Files

- `linux-7.2-tiny.config`: the exact kernel config that was built (tinyconfig plus
  64BIT, ELF, initramfs, the 8250 serial console, and RELOCATABLE).
- `limine.conf`: `protocol: linux`, with the kernel and initramfs as modules.
- `init/init.c`: the static `/init`. It prints a banner and asks for power-off.
- `build-linux-iso.sh`: packs the initramfs and writes the ISO, reusing the Limine
  binaries the Helix build already downloads.
- `evidence/serial-boot.log`: serial output from a headless QEMU run.

## Reproduce

1. Get a Linux source tree (this run used the `v7.2` tag, shallow-cloned from
   `torvalds/linux`, outside the repo).
2. `make O=../build tinyconfig`, then copy `linux-7.2-tiny.config` to `../build/.config`
   and run `make O=../build olddefconfig`.
3. `make O=../build -j2 bzImage`. Needs `flex`, `bison`, `libelf-dev` and `libssl-dev`.
4. From the repo root, with the Helix Limine binaries in `build/deps/limine-binary`:
   `sh experiments/linux-on-helix/build-linux-iso.sh <bzImage> linux.iso`
5. Boot headless with serial output:
   `qemu-system-x86_64 -machine q35 -m 512 -display none -no-reboot -accel tcg
   -drive if=pflash,format=raw,unit=0,readonly=on,file=OVMF_CODE_4M.fd
   -drive if=pflash,format=raw,unit=1,file=OVMF_VARS copy
   -cdrom linux.iso -serial file:serial.log`

Under TCG (no `/dev/kvm`) on a 2-core sandbox, the kernel build took about 2 minutes.

## What went wrong on the way (and the fixes)

- **Non-relocatable kernel**: Limine panicked with "Non-relocatable kernel could not
  be loaded at required address 0x1000000". Fixed with `CONFIG_RELOCATABLE=y`.
- **No output**: the first runs printed nothing on serial. The Linux console was not
  up yet, so `earlycon=uart8250,io,0x3f8` was added. Limine's `serial: yes` was added
  too.
- **Build**: `objtool` needed `libelf-dev`.

## Limits

- Power-off is not available in this config (no ACPI power management), so the
  kernel prints "System halted instead". The QEMU run then has to be ended by the timeout.
- Only the 8250 serial console and the i8042 keyboard are enabled. No disk, network or
  graphics driver. Helix's AHCI and network stack are not used.
- The Linux kernel is GPLv2, and Helix's licence is still undecided. Keep this on the
  experiment branch until that is settled.
