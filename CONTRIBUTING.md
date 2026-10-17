# Contributing to Helix OS

Helix is a long-term systems project. The most valuable contribution is a small,
correct, well-documented change that boots.

## The five rules

Every pull request must satisfy all of these:

1. **It compiles.** `make` finishes without errors.
2. **It boots.** `make test` passes — the kernel still reaches the banner.
3. **It is documented.** New behaviour is described in `docs/`, and the commit
   message explains *why*, not just *what*.
4. **It leaves the repository working.** Never merge a half-finished subsystem
   that breaks the boot.
5. **It explains what changed.** The PR description states what changed, why,
   and how it was tested.

If your change fails, that is normal and fine. Show the error, explain it, fix
it, and explain the fix. Do not hide failures or claim success you did not
observe.

## Getting set up

```sh
sudo apt-get install build-essential qemu-system-x86 ovmf gdisk mtools curl
make            # build
make test       # verify a boot
```

## What to work on

- Pick a milestone in [ROADMAP.md](ROADMAP.md) and check its prerequisites are
  done.
- Prefer completing the current milestone over starting the next one.
- Small, focused changes beat large rewrites.

## Coding style (C)

- C11 (`-std=gnu11`), freestanding. **No host libc in the kernel.**
- 4-space indentation, no tabs.
- `snake_case` for functions and variables; `UPPER_SNAKE` for macros and
  constants.
- `static` for anything not part of a module's public interface.
- Keep functions short; comment *why*, not *what*.
- Every header is include-guarded and has a one-line purpose comment.
- No floating point in the kernel (we build with `-mgeneral-regs-only`).
- Prefer explicit fixed-width types (`uint32_t`, not `unsigned`).

## Adding a source file

The kernel `Makefile` globs `kernel/src/*.c`, so a new file is picked up
automatically. Add its header to `kernel/include/` if it is shared.

## Commit messages

Use a short imperative subject and a body that explains the reasoning:

```
mm: add physical frame allocator

The bootloader hands us a memory map; we turn it into a bitmap of free
4 KiB frames. This is the first step of M3 and is not yet wired into page
tables.

Tested with: make test  (boot banner still reached)
```

## Documentation

- `docs/00-vision.md` — why Helix exists and what it is.
- `docs/01-boot.md` — how the current kernel boots, explained for newcomers.
- `docs/architecture.md` — the target architecture.

If you implement something, update the relevant doc and the status table in
`README.md`.

## Reporting problems

Include: what you ran, what you expected, what actually happened (paste the
real output), and what you already tried. Reproducibility is more useful than
urgency.
