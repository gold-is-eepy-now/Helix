# LOOM: experimental kernel architecture (simulator, milestone 1)

LOOM is a research architecture, not a replacement for the Helix kernel. This directory holds its
first deliverable: the core data structures, the kernel-side transition rules, a Weaver proposal
function, a simulator that runs one video-playback Continuity under resource pressure, and tests.

Scope, stated plainly:

- Host C only. Nothing here runs on hardware, boots, or is linked into the Helix kernel.
- The "frames" are counters and the resources are numbers. The demo shows decisions, not decoding.
- Phases 4 to 6 of the brief (kernel prototype, dynamic execution on hardware, real drivers) are not started.

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
