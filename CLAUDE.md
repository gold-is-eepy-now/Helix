# CLAUDE.md — guidance for AI agents working on Helix OS

This file is for any AI assistant (Claude or otherwise) contributing to Helix.
Read it fully before making changes.

## What Helix is

A from-scratch x86_64 operating system. Its **native** architecture is built on
object handles, capability-based security, message passing, and isolated driver
services. It is **not** Unix internally. Linux/BSD compatibility is a future
runtime layer, never the foundation.

## The one rule that matters

**Never pretend something works.** If you did not build it and boot it, do not
say it works. If a command failed, show the real output. If a subsystem is a
stub, say it is a stub. Honesty about state is more important than looking
productive.

## Milestone discipline

Before touching code, read [ROADMAP.md](ROADMAP.md). Work on the current
milestone; do not skip ahead. Every change must:

1. Compile.
2. Boot in QEMU (`make test`).
3. Be documented.
4. Leave the repository working.
5. Be explained.

When a build or boot fails: show the error, explain it, fix it, explain the fix.

## Build and test

```sh
make                     # build build/helix.img
make BUILD_DIR=/tmp/hb test   # build + boot headlessly and verify the banner
```

- `make test` is the source of truth for "does it boot". Never claim a boot
  without running it.
- In sandboxes where the working directory is a network mount, build out of
  tree with `BUILD_DIR=/tmp/...` or another local path: compilers and QEMU need
  a real filesystem, and produced binaries cannot be executed from a mount.
- QEMU in constrained sandboxes may need less RAM: `QEMU_MEM=512 make test`.

### The required sequence after every change

Every change, however small, must pass this exact sequence before it is
reported as done. Run it from the repository root, with the cross compiler on
`PATH`:

```sh
make clean && make test CC=x86_64-elf-gcc && make iso CC=x86_64-elf-gcc
```

- It must exit 0. Warnings are reported; a new warning in code you touched
  should be fixed, not ignored.
- `make test` runs the shell suite (`tests/shell-test.py`), the USB suite
  (`tests/usb-test.py`) and the HID unit tests. All must pass. Do not report a
  pass for a check you did not see pass.
- A failing build or test is reported with the real output. Do not describe a
  failure as "expected" or "unrelated" without showing why.

### Virtualisation: KVM when available, TCG otherwise

The tests boot Helix in QEMU (`qemu-system-x86_64`, machine `q35`, OVMF
firmware from `OVMF_CODE` / `OVMF_VARS_SRC`). Use hardware acceleration when
the host has it:

- If `/dev/kvm` exists and is writable, QEMU uses KVM. Pass `-accel kvm` (or
  let the harness detect it) for speed. Check with `ls -l /dev/kvm`.
- If `/dev/kvm` is absent, as in most sandboxes and containers, QEMU falls
  back to TCG software emulation. That is slower but correct. Do not treat the
  missing KVM as a build failure, and do not skip the tests because of it.
- Whichever accelerator runs, say which one in the report (for example,
  "make test passed under TCG, no /dev/kvm"). Never claim a KVM result on a
  TCG run.
- Hardware-specific behaviour (USB on the 2010 iMac 11,2) is not proven by
  either accelerator. QEMU results are evidence for the emulated controllers
  only.

### Housekeeping after every change

Each change that alters the kernel or the build bumps the version identity in
`kernel/src/version.h`. All four fields move together:

| Field | Rule | Example |
| --- | --- | --- |
| `HELIX_VERSION` | `MAJOR.MINOR.PATCH-dev.YYYYMMDD`; bump PATCH for each change | `0.5.5-dev.20261104` |
| `HELIX_CODENAME` | a new single word when a milestone or significant change lands; otherwise keep it | `Recall` |
| `HELIX_BUILD_DATE` | the current date, `YYYY-MM-DD`, matching the `dev.` date | `2026-11-04` |
| the development build string | the `dev.YYYYMMDD` part is the build string; it must match the build date | `dev.20261104` |

Also, in the same change:

- Update the docs the change affects: `README.md` status table,
  `ROADMAP.md`, `docs/*.md` for the subsystem, and the "Current state" list in
  this file. Do not list anything as implemented that was not built and booted.
- Add a shell command to `docs/02-shell.md`'s command table if you added one.
- Re-run the required sequence after the version bump, so the build the user
  receives carries the new version.
- Leave the working tree explained: list the files changed and the test results.
  Commit or push only when asked.

## Coding rules

- C11 freestanding. **No host libc in the kernel.**
- No floating point / SIMD in the kernel.
- Fixed-width integer types only (`uint32_t`, `int64_t`, ...).
- Keep modules small and single-purpose; comment the *why*.
- The kernel `Makefile` globs `kernel/src/*.c`; new files are picked up
  automatically.

## Layout

- `kernel/` — the kernel (C, freestanding). `src/`, `include/`, `linker.ld`.
- `drivers/` — native driver services (planned).
- `runtime/` — user-space runtimes, incl. future Linux/BSD layers (planned).
- `userspace/` — native programs and libraries (planned).
- `tools/` — image builder and QEMU runner.
- `tests/` — automated boot tests.
- `docs/` — vision, boot chain, architecture.

## Current state (update this when milestones land)

- **Implemented:** UEFI boot via Limine, x86_64 long-mode kernel entry, serial
  console (with ANSI colour and input), framebuffer text console (with cursor
  and colour), PS/2 keyboard input, the `helixsh` interactive shell, a command
  table of kernel-resident programs, TSC-calibrated uptime, a wall clock
  (Limine boot timestamp with a CMOS RTC fallback) for the `date` and `time`
  commands, Limine-backed
  memory map, CPU identification via CPUID, a bitmap physical frame allocator,
  a kernel heap (`kmalloc`/`kfree`), `panic()`/`kprintf()`, a freestanding
  string library, **our own four-level page tables (`vmm_map`/`vmm_unmap`/
  `vmm_protect`)**, an **IDT with readable CPU exception handlers**, and a
  **100 Hz PIT timer interrupt** (PIC remapped; LAPIC LINT0 opened for the
  8259).
- **Implemented:** the object manager with rights-bearing kernel handles
  (`object_*`/`handle_*`) and capability objects with derivation, revocation,
  and the unified `capability_allows()` authority gate (`capability_*`) — M5
  and M6, machine-tested via the `objtest` and `captest` shell commands.
- **Implemented:** kernel threads with a round-robin scheduler and timer
  preemption via saved context switches — M7, machine-tested via the boot and
  shell suites.
- **Implemented (M9):** `int $0x80` syscalls, ring-3 entry and exit,
  a console input syscall, an ELF64 loader for static `ET_EXEC` images, process
  teardown on exit, userland file I/O on the ext4 root (`open`/`read`/`write`/
  `close`/`lseek`, a per-process descriptor table, checked user pointers), a real
  `brk` heap, nested `execve` (spawn and wait) with argc/argv, `/bin` installed at
  boot from embedded programs (`/bin/init`, `/bin/sh`, `/bin/args`), and PID 1
  (`/bin/init`) starting `/bin/sh`, and a libc subset for ring-3 programs.
  Machine-tested.
- **Implemented:** shell output redirection (`>`, `>>`) and input redirection
  (`<`) in the kernel shell — a console capture sink collects the command's
  output and the shell writes it to ext4 with shared path resolution — plus
  ext4-backed `cat`/`ls`, machine-tested via `tests/shell-test.py`.
- **Implemented (environment, M9):** `envp` on the initial stack, libc
  `getenv`/`setenv`/`unsetenv`, kernel-shell `export`/`unset`, `/bin/env`.
  Machine-tested.
- **Implemented (kernel event log):** `klog` ring of scheduler, syscall, process,
  shell, input and exception events; `log` command; dumped on CPU exceptions.
  Machine-tested.
- **Implemented (desktop, basic, QEMU-tested only):** `desktop` command: teal
  desktop, My Computer, About, Terminal, Text Editor and Browser windows (the Browser loads plain http:// pages and shows them as text; the Terminal runs kernel shell
  commands and `exec <path>`, output shown in the window; keyboard-only programs
  are refused), taskbar with clock, Start menu, pointer
  driven by a PS/2 mouse (QEMU's default) or a USB mouse; dragging a title bar moves an outline and redraws the window on release; pointer moves redraw only the arrow; typing in the Text Editor redraws only its window. No compositor; not
  tested on real hardware.
- **Implemented (userland line editor, QEMU-tested only):** `/bin/edit FILE`, a ring-3 line editor (add, insert, replace, delete, print, save, quit) that reads and writes the file through the ordinary file calls.
- **Implemented (network, stage 1, QEMU-tested only):** Intel e1000 driver
  (`kernel/src/e1000.c`): PCI discovery, MMIO, descriptor rings, raw frame
  send/receive. The `net` command shows the MAC, link and counters; `net test`
  gets an ARP reply from QEMU's gateway. See docs/10-network.md.
- **Implemented (network, stages 2–3, QEMU-tested only):** Ethernet framing, ARP with
  a 16-entry cache, IPv4 send/receive with checksum and subnet routing, ICMP echo.
  The kernel-shell `ping <a.b.c.d>` and the ring-3 `/bin/ping` (via `SYS_NET_PING`) get
  replies from the QEMU gateway 10.0.2.2, with microsecond round-trip times. Not
  verified to the internet: this host's ICMP policy stops QEMU's user network from
  forwarding echo requests.
- **Implemented (network, stages 6–7, QEMU-tested only):** UDP with bound ports, a DNS
  A-record client (`nslookup`, ring-3 `/bin/nslookup` via `SYS_NET_RESOLVE`), and a DHCP
  client (`dhcp`) that replaces the static address when run. DNS over UDP to the internet
  is checked by hand (`github.com` resolves). No DHCP at boot or renewal.
- **Implemented (network, stage 8, QEMU-tested only):** TCP client (`kernel/src/net_tcp.c`):
  connect, in-order receive with a window, go-back-N send, close. Kernel `tcpget`
  fetched a 16 KB page from a host web server under QEMU. No listening sockets.
- **Implemented (network, stages 9–10, QEMU-tested only):** HTTP/1.1 GET client
  (`kernel/src/net_http.c`: Content-Length, chunked, redirects reported) with kernel
  `fetch`, and a desktop Browser window that shows plain http:// pages as text (checked
  with QEMU screenshots, not by the suite). No HTTPS yet.
- **Implemented (LC-1, Linux compat):** `compat/include/linux/*` headers (types, slab, spinlock, atomic, mutex, module_init, printk) and a self-test driver run at boot. See docs/08-linux-compat.md.
- **Implemented (Mac/Darwin compat):** `mac-darwin-compat/` subsystem with 64-bit Mach-O loader, Darwin BSD & Mach trap translation, Darwin process runtime (`darwin_runtime.c`), Darwin `launchd` PID 1 daemon with plist service loading and `launchctl` CLI (`launchd.c`), Mac-like case-insensitive filesystem, extended attributes (xattr), property list (.plist) engine, .app bundle resolution, CLI tools (`sw_vers`, `defaults`, `plutil`, `uname`, `otool`, `open`, `launchctl`), and AppKit/Cocoa GUI translation to Helix X11 server (`Calculator.app`, `TextEdit.app`), machine-tested (157 checks).
- **Implemented (DoneJS compat):** `donejs-compat/` freestanding C11 JavaScript runtime ported for Helix OS, featuring AST tree-walking interpreter, CommonJS modules (`require`), standard library (`path`, `fs`, `process`, `util`, `assert`, `events`, `buffer`, `os`, `stream`), Promises, microtasks, timers (`setTimeout`), and event loop draining. Machine-tested (hosted suite).
- **Not implemented:** the full C library (userspace has a libc subset in
  `userspace/libc`: string, stdlib, stdio, unistd, errno; there is no `fork`;
  the M9 exit gate is not yet passed), the `installer` / live install
  environment (needs a disk driver and partitioning), isolated driver services,
  a network stack above the HTTP client (HTTPS and TLS, listening sockets), a real graphics stack (the desktop is a basic framebuffer demo
  with no compositor), POSIX translation layer.

Do not write documentation or commit messages that imply unimplemented systems
exist.

## Teaching mode

The project owner is learning systems programming. When making a change, briefly
explain: what is being built, why, any new C or x86 concepts, and how to test it.
Keep explanations concrete and tied to the code.
