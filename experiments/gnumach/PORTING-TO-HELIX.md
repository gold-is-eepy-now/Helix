# GNU Mach as a Helix kernel core: experiment notes

Branch: `super-duper-experimental`. One-off experiment. **Nothing here builds or boots.**

## Provenance

- Source: `https://github.com/0xffea/gnumach` (a public fork), commit `2a6b4fc`.
  Savannah (`git.savannah.gnu.org`) was not reachable from the build sandbox, so this
  copy is not verified against the official tree.
- Copied: `kern/ ipc/ vm/ i386/ device/ ddb/ include/ util/` and `COPYING`, `README`,
  `DEVELOPMENT`, `ChangeLog`. Not copied: `linux/` (343k lines of Linux driver code),
  `xen/`, the autotools build files and git history.
- Licence: GNU GPL (see `COPYING`). The Helix licence is still an open owner decision
  (see `docs/` and the project notes), so this code must stay on the experiment branch
  until that is settled.

## Size and shape (measured)

| Directory | Files | Lines of C/H |
|---|---|---|
| kern | 82 | 29,400 |
| ipc | 42 | 18,994 |
| vm | 25 | 18,281 |
| i386 | 122 | 26,093 |
| device | 31 | 9,322 |
| ddb | 41 | 8,769 |

The core is about 90,000 lines. None of the copied files mention x86_64. The code is
i386 (32-bit protected mode, two-level paging, 32-bit `unsigned long` assumptions).

## What a real port needs

1. **Build system.** GNU Mach uses autotools and its own Makefile fragments. Helix uses a
   plain Makefile for `kernel/`. A new build step would have to drive GNU Mach's
   compiler flags into Helix's `kernel/Makefile` or a sub-make.
2. **MIG.** Mach IPC stubs come from `.defs` files compiled by the Mach Interface
   Generator (MIG). There are 24 `.defs` files here. MIG is a separate host tool, so it
   would have to be built first and kept in the toolchain.
3. **32 to 64 bits.** Page tables (PAE or 64-bit long mode), pointer and `long` sizes,
   the GDT and IDT setup, and the SYSENTER/SYSCALL paths all change. Helix already has
   long-mode page tables, so the VM layer could be replaced rather than converted.
4. **Boot handoff.** Helix boots through Limine. GNU Mach expects a Multiboot-style
   handoff, so either a Limine-to-Multiboot shim or new boot code is needed.
5. **Conflict with Helix's design.** Helix already has an object manager, capability
   checks and its own IPC direction (`CLAUDE.md`). Mach ports would replace the object
   model, so the port means deciding which of the two designs wins.

## Suggested first slices (not started)

1. Build GNU Mach's `kern/` and `i386/` objects on the host with `-m32` to confirm the
   source compiles on this toolchain.
2. Write a Limine entry stub that reaches GNU Mach's `kernel_main` with a minimal
   memory map, and stop at the first missing piece.
3. Port the physical memory and page-table layer to x86_64.
4. Generate MIG stubs for one interface (for example `host`) and run one IPC round trip.

Slice 1 is the cheapest check on whether the path is viable at all.
