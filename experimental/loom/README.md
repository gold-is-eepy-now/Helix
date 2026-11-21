# LOOM: experimental kernel architecture (simulator, milestone 1)

LOOM is a research architecture, not a replacement for the Helix kernel. This directory holds the
core data structures, the kernel-side transition rules, a Weaver proposal function, a host simulator,
host tests, and a bootable LOOM kernel that runs the same scenario inside the kernel.

Scope, stated plainly:

- The LOOM kernel boots only under QEMU with OVMF (TCG, no KVM on the build machine). Nothing is
  tested on real hardware, and it is not linked into the Helix kernel.
- The "frames" are counters and the resources are numbers. The demo shows decisions, not decoding.
- Not in the kernel yet: memory management beyond reading Limine's memory map, isolation of Cells
  (no page tables, no user mode), a capability system beyond bitmasks, typed communication, and event
  dispatch. These are Phase 4 items of the brief. Phases 5 and 6 are not started.

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

Expected serial output ends with `LOOM boot OK`, then the scenario's lines, then `LOOM demo: PASS`,
and the target prints `boot-test: PASS`. The kernel is built with `gcc` (`KCC`) because the machine has
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

- No kernel code, no boot, no QEMU run. Nothing here is evidence about Helix or any hardware.
- No Cells as separate execution units; no feedback loops, buffers, backpressure or deadlock detection.
- No multithreading; the library is not re-entrant.
- Capabilities are a bitmask, not Helix's object capabilities.
