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
| Memory management, heap | ⛔ not started (M3) |
| Scheduler and user space / ELF / syscalls | ⛔ not started (M4–M9) |
| Object manager & capabilities | ⛔ not started (M5–M6) |
| Isolated driver services, filesystems, networking | ⛔ not started (M10–M12) |
| Linux / BSD / POSIX compatibility | ⛔ not started (much later) |

See [ROADMAP.md](ROADMAP.md) for the full plan and [docs/02-shell.md](docs/02-shell.md)
for a walkthrough of the current shell.

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

### Requirements

- `gcc` (with 64-bit support) and `binutils`
- `make`, `curl`
- `qemu-system-x86_64` + UEFI firmware (`OVMF`)
- `gdisk` (`sgdisk`) and `mtools` (`mformat`, `mmd`, `mcopy`) to build the disk
  image

On Debian/Ubuntu:

```sh
sudo apt-get install build-essential qemu-system-x86 ovmf \
                     gdisk mtools curl
```

### Commands

```sh
make            # build the bootable disk image (build/helix.img)
make kernel     # build just the kernel ELF
make run        # boot the image in QEMU (serial output on your terminal)
make test       # boot headlessly and verify the boot banner (CI-style)
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

[helix] console: serial online + framebuffer 1280x800x32
[helix] arch: x86_64, long mode
[helix] state: entering safe idle loop
```

`make test` boots the image, captures that serial output, requires the three
banner lines, and saves a screenshot of the framebuffer console to
`build/test/screen.png`.

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
