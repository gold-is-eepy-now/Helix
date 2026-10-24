# Doom port experiment

## Decision and scope

Keep the Doom work as groundwork and defer a playable port until Helix has a
userland execution path. The Doom engine will be a userspace program, not code
linked into the kernel. Revisit it after M9 provides the native ELF loader,
syscall ABI, and initial runtime; add graphics, input, timing, and memory
interfaces to that runtime as needed.

## What works now

- DoomGeneric is present at a pinned upstream revision, with its upstream
  license in `third_party/doomgeneric/LICENSE`.
- The image builder stages FreeDoom Phase 1 (`freedoom1.wad`) as a Limine
  module and copies its BSD-3-Clause license notice into `/boot/doom`.
- The kernel recognizes an `IWAD` header in a Limine module and exposes its
  memory through `kernel/src/doom_wad.h`.
- Helix's framebuffer presenter and PS/2 key-event API are potential platform
  interfaces for a future userspace port.

These checks confirm the asset reaches the kernel. They do not mean that the
Doom engine is linked, launched, or playable. The engine still depends on
hosted C and OS services, and no Doom launch command exists.

## Game data and licensing

FreeDoom Phase 1 v0.13.0 is used as the redistributable Doom 1-compatible game
data. Its assets are under the 3-clause BSD license; the required notice is
`third_party/freedoom/COPYING.adoc`. DoomGeneric is separately licensed under
GPL-2.0-or-later. Keep both notices with their respective components. See the
[FreeDoom project](https://github.com/freedoom/freedoom), [v0.13.0
release](https://github.com/freedoom/freedoom/releases/tag/v0.13.0), and
[DoomGeneric upstream](https://github.com/ozkl/doomgeneric).

The image build uses a root-level `freedoom1.wad` when present; otherwise it
downloads the pinned FreeDoom archive and checks its SHA-256. WAD binaries are
ignored by Git. Use `DOOM_WAD=/path/to/another.wad` to select a different IWAD,
or `DOOM_WAD=` to build without an IWAD. The selected file is placed in the
image as `/boot/doom/doom1.wad` and passed to the kernel by Limine.

## Resume point

After M9, port DoomGeneric as a userspace application. Before calling it
playable, integrate its renderer, keyboard, timer, allocation, and WAD access
with Helix's userspace runtime and boot-module interface, then verify real
gameplay under QEMU.
