# Helix OS

**A from-scratch x86_64 operating system whose native architecture is built on
object handles, capabilities, message passing, and isolated driver services —
not on Unix.**

Helix is not Linux, not BSD, and not Unix internally. It is a long-term,
open-source research OS. Linux and BSD compatibility are planned as *runtimes
that run on top of the Helix kernel*, translating Unix behaviour into Helix's
native object model. They are not the foundation.

> Status: **Milestone 2 complete — interactive shell with built-in
> commands (help, sysinfo, cpu, mem, uptime, calc, echo, ...) and a typed bus
> to it over serial or PS/2, machine-tested.** M1 is also part of that: the
> kernel boots, prints a banner, and has a real input system on top.

---

## What is implemented today

| Area | State |
|------|-------|
| UEFI boot (OVMF + Limine) | ✅ working, machine-tested |
| x86_64 kernel, long mode | ✅ working |
| Serial console (16550 UART) | ✅ working (input + output, ANSI colour) |
| Framebuffer text console (8×8 font) | ✅ working (cursor, backspace, colour) |
| PS/2 keyboard input | ✅ working (polling, US layout, shift) |
| Interactive shell (`helixsh`) | ✅ working, machine-tested |
| Kernel-resident commands (help, sysinfo, cpu, mem, uptime, ...) | ✅ working |
| Time source (TSC calibrated against the PIT) | ✅ working |
| Calibrated time, memory map, firmware detection | ✅ working |
| Kernel heap (`kmalloc`/`kfree`) + panic/`kprintf` | ✅ working (M3) |
| Physical frame allocator (bitmap) | ✅ working, machine-tested (M3) |
| Page tables, guard pages | ⛔ not started (M3 remainder) |
| Scheduler and user space / ELF / syscalls | ⛔ not started (M4–M9) |
| Object manager & capabilities | ⛔ not started (M5–M6) |
| Isolated driver services, filesystems, networking | ⛔ not started (M10–M12) |
| Linux / BSD / POSIX compatibility | ⛔ not started (much later) |

See [ROADMAP.md](ROADMAP.md) for the full plan, [docs/02-shell.md](docs/02-shell.md)
for the shell, and [docs/03-cpython-requirements.md](docs/03-cpython-requirements.md)
for what it will take to run CPython and Linux/BSD software.

See [ROADMAP.md](ROADMAP.md) for the full plan.

---

## Repository layout

```
helix/
├── kernel/        the kernel itself (C, freestanding)
│   ├── src/       kernel sources
│   ├── include/   kernel headers (incl. the vendored limine.h)
│   ├── linker.ld  link layout: where code/data land in memory
│   └── Makefile
├── drivers/       native driver services (planned)
├── runtime/       user-space runtimes, incl. future Linux/BSD layers (planned)
├── userspace/     native user-space programs and libraries (planned)
├── tools/         build/boot helpers (image builder, QEMU runner)
├── tests/         automated boot tests
├── docs/          architecture, boot chain, and teaching notes
├── README.md
├── ROADMAP.md
├── CONTRIBUTING.md
└── CLAUDE.md
```

---

## Building and running

### Just want to run it?

You do not need a compiler to run Helix. A prebuilt bootable image is
available as a downloadable attachment: unzip it, install QEMU, and run
`./run.sh`. The only requirement is QEMU (`brew install qemu` on macOS,
`sudo apt install qemu-system-x86 ovmf` on Linux).

The image is **UEFI-only** (pure GPT, no legacy BIOS code). It runs in QEMU on
any host, and in VirtualBox on x86_64 hosts with EFI enabled (`--firmware efi`).
VirtualBox on Apple Silicon cannot run x86 guests; use QEMU/UTM there instead.

### Requirements

- A C toolchain that can emit x86_64 **ELF** (see the platform notes below)
- `make`, `curl`
- `qemu-system-x86_64` + UEFI firmware (`OVMF`/EDK2)
- `gdisk` (`sgdisk`) and `mtools` (`mformat`, `mmd`, `mcopy`) to build the disk
  image

On Debian/Ubuntu:

```sh
sudo apt-get install build-essential qemu-system-x86 ovmf \
                     gdisk mtools curl
```

On macOS:

```sh
brew install x86_64-elf-gcc qemu gdisk mtools
```

Apple's toolchain **cannot link ELF** — its `ld` is Mach-O only — so the
kernel cannot be built with the `clang`/`ld` that ship with Xcode. Install the
cross-compiler and point the build at it:

```sh
make CC=x86_64-elf-gcc test
```

(`make TOOLCHAIN=llvm` is also supported if you prefer `brew install llvm`.)
Firmware paths differ on macOS; `tools/ovmf.sh` finds them automatically.

### Commands

```sh
make            # build the bootable disk image (build/helix.img)
make iso        # build a bootable UEFI CD image (build/helix.iso)
make disks      # build VDI/VMDK/VHD hypervisor disk images
make kernel     # build just the kernel ELF
make run        # boot the image in QEMU (serial output on your terminal)
make test       # boot headlessly and drive the shell (CI-style)
make clean      # remove build output
```

Everything is built into `build/` by default. You can redirect it:

```sh
make BUILD_DIR=/tmp/helix-build test
```

### What a successful boot prints

```
HELIX OS
Native Object Kernel
Boot successful.

Helix OS 0.2.0 "Kernel Shell"

[helix] console: serial + framebuffer
[helix] memory:  462 MiB usable
[helix] pmm:     510 MiB total, 461 MiB free (118064 frames)
[helix] heap:    260 KiB reserved
[helix] clock:   TSC calibrated

Type 'help' for a list of commands, 'sysinfo' for system info.

helix> 
```

`make test` boots the image, drives the shell over a serial socket, checks 23
behaviours (banner, prompt, `help`, `echo`, `calc`, `cpu`, `mem`, `pmm`,
`heap`, `memtest`, `uptime`, `sysinfo`, error handling, `color`), and audits
the framebuffer screenshot for readability. It also saves that screenshot to
`build/test/shell-screen.png`.

---

## Development philosophy

Every milestone must:

1. **Compile.**
2. **Boot in QEMU.**
3. **Be documented.**
4. **Leave the repository working.**
5. **Explain what changed.**

When something fails we show the error, explain it, fix it, and explain the fix.
We never pretend something works. If a subsystem is not implemented, the docs
say so.

---

## Design principles

- **Everything is an object.** Processes, threads, memory regions, display
  surfaces, network interfaces, storage volumes, channels and devices are all
  objects.
- **Handles, not file descriptors.** Applications receive opaque handles to
  objects, not Unix fds.
- **Capabilities, not root.** Authority is explicit: `Display.Draw`,
  `Network.Connect`, `Storage.Read`, `Storage.Write`, `Driver.Control`.
  Least privilege is the default.
- **Message passing.** Objects communicate by sending messages through
  channels.
- **Isolated drivers.** Drivers are user-space services wherever practical.
- **Native first.** A clean native object API comes before any compatibility
  layer.

Read [docs/00-vision.md](docs/00-vision.md) for the full rationale and
[docs/01-boot.md](docs/01-boot.md) for a guided tour of how the current kernel
boots.

---

## License

TBD by the project owner. Third-party components retain their own licenses:
`limine.h` is 0BSD, and the embedded 8×8 font (`kernel/src/font8x8.h`) is
public domain.
