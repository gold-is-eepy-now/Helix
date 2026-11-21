# LOOM: experimental kernel architecture (simulator, milestone 1)

LOOM is a research architecture, not a replacement for the Helix kernel. This directory holds the
core data structures, the kernel-side transition rules, a Weaver proposal function, a host simulator,
host tests, and a bootable LOOM kernel that runs the same scenario inside the kernel.

Scope, stated plainly:

- The LOOM kernel boots only under QEMU with OVMF (TCG, no KVM on the build machine). Nothing is
  tested on real hardware, and it is not linked into the Helix kernel.
- The "frames" are counters and the resources are numbers. The demo shows decisions, not decoding.
- Booted in the kernel (QEMU only): physical frames and per-Cell page tables; Cells in ring 3 with
  fault isolation; typed channels with capability handles; C Cells loaded from flat images; the
  Weaver running as a separate Cell that proposes through a syscall, with the kernel deciding.
- Not in the kernel yet: preemptive scheduling (Cells run one at a time, cooperatively), event
  queues beyond per-tick delivery, Continuity registration owned by Cells (the registry is kernel
  code), and the 39 host tests (they run only on the host). Phases 5 and 6 are not started.

## Reproduce

Requires a C compiler (`cc` or set `HOST_CC`). No other tools.

```sh
cd experimental/loom
make test   # build and run the host tests: 39 checks
make demo   # build and run the simulator
```

Expected: `make test` ends with `loom-test: 39 checks, 0 failed` and exits 0. `make demo` ends with
`result: PASS` and exits 0. The build uses `-Werror`; any new warning fails it.

To check memory and undefined behaviour, build the two programs with
`-fsanitize=address,undefined -fno-sanitize-recover=all` (the commands are the same as the Makefile's
with that flag added).

## Kernel boot (QEMU)

Uses Limine, the bootloader Helix already downloads into `build/deps/limine-binary` (run `make deps`
in the Helix tree first). Needs `xorriso` and OVMF.

```sh
cd experimental/loom
make -f Makefile.kernel boot-test   # build the kernel and ISO, boot in QEMU, check the serial output
```

The target checks for these success lines in order: `LOOM boot OK`, `LOOM mm: PASS`,
`LOOM cells: PASS`, `LOOM channels: PASS`, `LOOM user cells: PASS`, `LOOM weaver cell: PASS`,
`LOOM demo: PASS`, and then prints `boot-test: PASS`. The kernel is built with `gcc` (`KCC`) because the machine has
no `x86_64-elf-gcc`. Its freestanding flags are in `Makefile.kernel`. Files: `kernel/linker.ld`,
`kernel/src/boot.c` (Limine requests and entry), `serial.c` (COM1), `string.c`, `kassert.c`, `demo.c`.
The rules themselves are the shared files in `src/`, compiled unchanged.

## Layout

| Path | Contents |
| --- | --- |
| `include/loom.h` | core types and the public rules |
| `src/field.c` | Resource Field accounting (admission, overcommit, rollback) |
| `src/contract.c` | correctness contract rule |
| `src/morph.c` | Morphology demand sums; result strings |
| `src/continuity.c` | start, stop, and the transition rules |
| `src/weaver.c` | read-only Weaver proposal |
| `src/sim_video.c` | the simulator: video Continuity, pressure events, one injected failure, one rogue proposal |
| `tests/test_loom.c` | host tests, including the rejected-transition checks |
| `docs/ARCHITECTURE.md` | the Phase 1 specification, with open questions and the parts not done |
| `docs/RESEARCH.md` | prior-art notes, with what was checked and what was not |

## What the demo shows

`make demo` prints one line per event. In order:

1. Start in Morphology A (hardware decode), chosen by the Weaver.
2. Tick 8: the GPU is withdrawn. The Field is overcommitted. The Weaver moves to B (software decode).
   State is kept (frames and reference frame unchanged, schema migrated).
3. Tick 14: a thermal throttle. The Weaver moves to C (half resolution, quality 60, a permitted degradation).
4. Tick 18: the throttle clears, but the transition back to B is refused once by an injected migration
   failure. The state and reservation are unchanged. Tick 19: the retry succeeds.
5. Tick 24: a rogue proposal of Morphology D (quality 30) is refused by the contract rule.
6. Tick 26: the GPU returns. The Weaver moves back to A.

Summary: 32 ticks, 32 frames delivered, 0 stalls, 4 transitions, 2 refused (one migration, one
rogue), 1 contract refusal. The demo exits non-zero if any frame stalls or the contract refusal count
is not exactly one.

## Not done

- Nothing here is evidence about Helix or any real hardware. Everything kernel-side ran only under QEMU/TCG.
- No preemption: one Cell runs at a time, and a Cell runs only when the kernel resumes it.
- No feedback loops, buffers, backpressure or deadlock detection.
- No multithreading; the library is not re-entrant.
- Capabilities are kernel handles with rights (SEND, RECV, PROPOSE), not Helix's object capabilities.
- Loader limits: at most 16 code pages per Cell image, no bss in Cell images, physical memory used only below 4 GiB.
