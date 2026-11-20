# LOOM architecture (Phase 1 specification)

Status: a host-side model and simulator. Nothing here runs on hardware, and no LOOM code is part of
the Helix kernel. Phases 4 to 6 of the brief (kernel prototype, dynamic execution on hardware, real
drivers) have not started. Where this document describes something the simulator does not do, it says so.

## 1. Philosophy, and where it may be wrong

The brief's claim is that the kernel should manage behaviours and their outcomes, not programs and
their execution. A Continuity is the unit: a requested behaviour with a contract. The implementation
(a Morphology) may change while the contract holds.

Challenges to this, kept open on purpose:

- **Redundancy.** A Continuity with several Morphologies looks like a process with a policy that
  picks a code path. The difference has to be the enforced contract plus the state migration rule.
  If the enforcement is only a convention in the policy, the abstraction is redundant. The simulator
  makes the enforcement real: the kernel rules refuse a transition that breaks the contract, whatever
  the Weaver proposes.
- **Morphology explosion.** Declaring every valid arrangement up front may be impractical. A
  Continuity with a parameterised decoder (any resolution, any codec) is better described by a
  family than by a list. The simulator uses a fixed list of at most four Morphologies.
- **Migration cost.** Switching hardware decoder to software decoder changes state layout. The
  simulator assumes a migration function exists and costs nothing. Real cost is unmeasured.

## 2. Kernel responsibilities

The kernel in this design owns the rules, and nothing else:

1. Memory isolation (not modelled; the simulator has one address space).
2. Capability authority checks (modelled: a 32-bit mask per Continuity; a Morphology names the bits it needs).
3. Cell lifecycle (declarative only; see section 4).
4. Continuity enforcement: contract checks, admission, and transition rules (modelled, in `continuity.c`).
5. Typed communication (not modelled).
6. Event dispatch (not modelled; the simulator advances in ticks).
7. Resource accounting (modelled: the Resource Field in `field.c`).
8. Hardware mediation (not modelled).
9. Scheduling primitives (not modelled beyond the tick loop).
10. Safe Morphology transitions (modelled in `continuity.c`).

Items 1, 5, 6, 8 and 9 are the traditional parts. The brief asks why they remain necessary. Memory
isolation remains because a Cell's state must not be reachable from another Cell. Typed communication
remains because a Morphology's Cells must talk to each other and to the Continuity's inputs and
outputs. Hardware mediation remains because only the kernel can make the final decision about DMA and
device registers. These are not LOOM inventions.

## 3. Continuity

A Continuity declares:

| Field | Meaning | In the simulator |
| --- | --- | --- |
| name | identity | `video-playback` |
| contract | correctness class, minimum quality, maximum stall ticks | DEGRADABLE, min 50%, 0 stalls |
| capability authority | bits it holds | `CAP_GPU_DECODE` |
| Morphologies | the permitted implementations | four (A, B, C, D) |
| state | bytes and a schema version | 8 bytes base, schema-dependent extension |

Correctness classes:

- **Exact:** the output must equal the reference. Only quality 100 is allowed.
- **Degradable:** quality may fall to a declared minimum. Quality below it is an unacceptable failure.
- **Timing:** `max_stall_ticks` bounds the number of consecutive ticks without a delivered frame.
  A stall beyond the bound is reported as a timing contract violation.

The brief's examples map onto these classes. Reducing video resolution is Degradable (quality 60 in
Morphology C). Corrupting a cryptographic operation would be Exact, and no degraded Morphology may
serve it. The simulator enforces the first kind; the second is covered by the Exact rule but not
demonstrated.

## 4. Cells

A Cell is a named unit with a demand vector: the resources it consumes. The simulator models Cells
only as declarations used to compute a Morphology's demand. They have no private execution state,
no typed ports, and no lifecycle of their own. The brief asks that Cells not be renamed threads or
processes. This design does not yet do that; it is the largest open gap. Cell lifecycle (create,
run, quiesce, destroy) and their isolation are specified here, not implemented:

- **Create:** the kernel allocates the Cell's resource reservation as part of its Morphology's admission.
- **Run:** a Cell runs only while its Morphology is the Continuity's current one.
- **Quiesce:** a transition first quiesces the old Cells at a frame boundary, then migrates state.
- **Destroy:** a Cell's reservation is released when the Morphology it belongs to is no longer current.

## 5. Morphology

A Morphology is a set of Cells (at most four), with:

- `quality`: percent of the reference output, 0..100
- `state_schema`: the layout version of the Continuity state it reads and writes
- `required_caps`: capabilities the Continuity must hold to use it
- `demand`: the sum of its Cells, computed by `loom_morph_finalize`

The Continuity lists its permitted Morphologies. Listing is the permission: an arrangement that is
not listed cannot be selected, whatever the Weaver proposes.

## 6. Resource Field

The Field holds, for each resource kind, a capacity and a reserved total. The kinds are:

| Kind | Unit | Simulated |
| --- | --- | --- |
| CPU | milli-CPU | yes |
| GPU | milli-GPU | yes |
| memory | KiB | yes |
| memory bandwidth | KiB per tick | yes |
| power | mW | yes |
| storage bandwidth, network bandwidth, DMA, thermal | various | declared, not simulated |

Rules:

- **No conversion.** There is no function that turns one kind into another. A demand fits only when
  every kind fits. A GPU demand is never met by spare CPU.
- **Admission is all-or-nothing.** `loom_field_reserve` checks every kind before it adds anything.
- **Budgets and reservations.** A reservation is the demand a Continuity holds. The simulator does
  not yet model budgets that are smaller than a reservation (rate limits); that remains open.
- **Overcommit.** A capacity can fall below what is reserved (a GPU withdrawn, a thermal throttle).
  The kernel does not revoke reservations automatically. The Field reports overcommit, and the
  Weaver must move Continuities out. Until one moves, the Continuity stalls. This is a deliberate
  choice: revocation without a Morphology to fall back to would break the contract at once.

## 7. The Weaver

The Weaver is read-only. `loom_weaver_propose` looks at the Field and a Continuity and returns the
best Morphology that would be admitted now, after the Continuity gives up its own reservation. "Best"
is the highest quality, then the lowest CPU demand. The Weaver is policy, so the rule can change.

The Weaver cannot bypass the kernel. Every proposal goes through `loom_transition`, which runs the
same checks. The simulator demonstrates this with a rogue proposal (Morphology D at tick 24), which
is refused with a contract error. Also, `loom_weaver_propose` never returns a Morphology that breaks
the contract (tested).

Where the Weaver should live: in a privileged userspace service, not the kernel. The current code is
a library function called in-process by the simulator, because there is no userspace yet. The
boundary is a function call today. It must become a message interface before the Weaver can run
outside the kernel.

## 8. Security model

- A Continuity holds a capability mask. A Morphology names the bits it needs. A transition to a
  Morphology whose bits are not all held is refused (tested: `LOOM_ERR_CAPABILITY`).
- Capabilities here are a bitmask in the model. Helix's real capability system (see the Helix docs)
  is not used. Unforgeable handles, derivation and revocation are not modelled.
- The Weaver holds no capabilities of its own that could change the Field. It reads the Field.

## 9. Scheduling model

The simulator is tick-based. Each tick, in order:

1. Pressure events change the Field's capacities.
2. The Weaver proposes; if the proposal differs from the current Morphology, the kernel attempts the transition.
3. A frame is delivered if the Field is not overcommitted; otherwise the Continuity stalls.

There is no preemption, no priority among Continuities, and no CPU-level scheduling. Feedback loops,
buffers, backpressure, deadlock detection and event ordering (brief section 5) are not implemented.
They need a design of their own. The open questions are in RESEARCH.md.

## 10. Hardware abstraction

Not implemented. The Morphology tables are written by hand for one simulated machine. The brief asks
that applications not see the hardware. In this model, the Continuity sees only its Morphologies, and
the Field sees only capacities, so the abstraction is present in form. Whether it holds once real
devices (x86_64, ARM64, GPUs, FPGAs) must be shown in Phase 6.

## 11. Failure handling

A transition is a sequence of checks. The first failure refuses it and leaves the Continuity, its
state and the Field unchanged:

| Failure | Result | Tested |
| --- | --- | --- |
| target does not exist, or is the current one | `BAD_TARGET`, `SAME` | yes |
| contract would break | `CONTRACT` | yes |
| capability missing | `CAPABILITY` | yes |
| admission fails | `NO_RESOURCES`, old reservation restored | yes |
| state cannot migrate | `MIGRATION`, state and reservation restored | yes (injected) |

Rollback is by construction: migration works on a copy of the state, and the reservation is restored
with `loom_field_restore`, which skips admission because the Field may already be overcommitted.

Not handled: a crash in the middle of a transition (the simulator is single-threaded and cannot
crash mid-step), concurrent transitions, and a migration that succeeds but produces wrong state.
The last one cannot be detected by the kernel without a check defined by the Continuity's contract.

## 12. Memory ownership and synchronisation (implementation rules)

- The Continuity owns its state bytes. The migration callback receives a copy and must not keep a
  pointer to it. The Field owns the reserved totals. The Morphology tables are owned by the
  Continuity and are read-only after setup.
- There is no synchronisation: the library is single-threaded and not re-entrant. Any future
  multithreaded use needs a lock around the Field and each Continuity.
- Allocations: none in the library. The simulator uses static storage.
