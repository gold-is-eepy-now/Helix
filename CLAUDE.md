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
  table of kernel-resident programs, TSC-calibrated uptime, Limine-backed
  memory map, CPU identification via CPUID, a **bitmap physical frame
  allocator** and a **kernel heap** (`kmalloc`/`kfree`), `panic()` and
  `kprintf()`, and a freestanding string library.
- **Not implemented:** page tables / guard pages, real interrupt handling, a
  scheduler, user space, ELF loading, syscalls, the object manager,
  capabilities, isolated driver services, filesystems, networking, graphics,
  compatibility layers.

Do not write documentation or commit messages that imply unimplemented systems
exist.

## Teaching mode

The project owner is learning systems programming. When making a change, briefly
explain: what is being built, why, any new C or x86 concepts, and how to test it.
Keep explanations concrete and tied to the code.
