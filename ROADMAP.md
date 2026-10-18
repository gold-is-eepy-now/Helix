# Helix OS — Roadmap

This roadmap is deliberately incremental. Each milestone is small enough to
finish, boot, and document. **Do not start a milestone before its prerequisites
are complete, boot-tested, and documented.** A later milestone may be completed
early when it is independently useful, but that does not close unfinished work
in an earlier milestone. The kernel does not become more real by writing more
code faster; it becomes more real by always working.

Legend: ✅ done · 🚧 in progress · ⛔ not started

---

## M0 — Repository and toolchain ✅

- Project skeleton, build system, QEMU/UEFI test harness.
- Reproducible `make test` that proves a boot.

## M1 — Boot and console ✅

- UEFI boot via Limine on x86_64.
- Kernel entry in long mode.
- Serial console + framebuffer text console.
- Banner: `HELIX OS / Native Object Kernel / Boot successful.`
- Safe halt loop.

**Out of scope (intentionally):** networking, USB, GUI, filesystems, package
management.

## M2 — Interactive shell ✅

The kernel stops being a silent demo and becomes something you can drive by
typing.

- Input: serial (COM1) polling + PS/2 keyboard (scancode set 1, US layout).
- Line editor: printable chars, Backspace, Ctrl-C, Ctrl-L, Enter.
- Console gains an ANSI-on-serial, packed-RGB-on-framebuffer colour API, a
  visible cursor on the framebuffer, `clear`, `backspace`, `puts/putc/cursor`.
- Real time source: TSC calibrated against the PIT (channel 2, mode 0) so we
  have millisecond uptime without interrupts.
- Memory map from Limine: total / usable RAM and a row-by-row range table.
- CPU identification: vendor (CPUID 0) and brand (CPUID 0x80000002–4).
- Firmware detection (UEFI vs BIOS) via the Limine firmware-type request.
- Kernel-resident command table (`help`, `sysinfo` / `neofetch`, `cpu`, `mem`,
  `uptime`, `version`, `echo`, `calc`, `clear`, `halt`, `reboot`).
- `sysinfo` renders a Neofetch-style panel beside a small ASCII logo with
  coloured logo strands and labelled values.
- Automated test harness (`tests/shell-test.py`) connects over the serial
  Unix socket, types real commands and verifies real responses.

Adding a new "program" is two steps: write a function with the command
signature and add one row to `helix_commands` in `kernel/src/commands.c`.

**Out of scope (intentionally):** dynamic memory, interrupts, scheduling,
user space, ELF loading, real filesystems, command history / completion.

---

## M3 — Kernel runtime foundations and memory management ✅

The kernel becomes a real program that owns physical and virtual memory.

**Runtime foundations** (the scaffolding everything else uses):

- ✅ Panic machinery: `panic()` prints and halts instead of hanging silently.
- ✅ `kprintf`-style formatter (`%s %c %d %u %x %p`, `l`/`ll` modifiers).
- ✅ A freestanding string/memory library (`memcpy`, `memset`, `strlen`, ...).
- ✅ A consistent `[helix]` boot-log prefix.

**Physical memory:**

- ✅ A bitmap physical frame allocator over the Limine memory map, with
  single-frame and contiguous-run allocation, and statistics (`pmm` command).

**Kernel heap:**

- ✅ `kmalloc` / `kzalloc` / `kfree`: first-fit free list with splitting and
  coalescing, growing on demand by requesting contiguous frames from the PMM
  and accessing them through the Limine higher-half direct map.
- ✅ Self-test (`memtest` command) that allocates, pattern-checks and frees.

**M3 exit-gate implementation:**

- ✅ A dynamically sized frame bitmap based on the highest usable physical
  address; its backing frames are reserved and counted in allocator stats.
- ✅ A dedicated 32 KiB kernel stack with an unmapped guard page and NX
  mappings; deliberate reads and execution attempts fault and recover through
  the M4 page-fault handler.

**M3 exit gate (passed):** the PMM sizes its bitmap from the boot memory map
without silently truncating usable memory; allocator statistics account for
its backing storage. The dedicated stack has an unmapped guard page and NX
mapping. QEMU reports and recovers from deliberate boundary and execute
faults; all 38 shell checks pass.

**Sequencing note:** page tables were pulled forward into M4 and are complete.
M5 object and handle APIs were started early; work now continues at M6.

## M4 — Interrupts, exceptions, timers and paging ✅

- ✅ Our own page tables: `vmm_map` / `vmm_unmap` / `vmm_protect` / `vmm_get_mapping`,
  walking the four levels, allocating tables from the PMM, splitting 2 MiB
  huge pages, and flushing the TLB with `invlpg`.
- ✅ IDT with assembly stubs for the 32 CPU exceptions and 16 IRQs, a uniform
  interrupt frame, and readable exception reports (vector, name, error code,
  RIP/CS/RFLAGS, CR2 for page faults).
- ✅ PIC remap to vectors 32–47, plus mask/unmask/EOI helpers.
- ✅ PIT channel 0 at 100 Hz driving IRQ0, with a tick counter and derived
  uptime.
- ✅ Local-APIC routing so 8259 interrupts reach the CPU (LINT0 ExtINT, TPR 0).
- ✅ New commands: `vmtest`, `ticks`, `swint`, `pitcount`, `fault`.
- ✅ The boot and shell suite covers the M4 paging, timer, and exception paths;
  M3, M5 and initial M6 checks have since expanded it to 45 checks.

**Still to do:** APIC/IOAPIC timers; per-process address spaces are being
introduced in M9 on top of the completed object manager.

## M5 — The object manager ✅

- ✅ The first real Helix concept: a global object table.
- ✅ Object types with reference counting.
- ✅ Rights-bearing handles with stale-handle rejection and rights-reducing
  duplication; the current kernel handle table is ready to move into processes.
- ✅ A native kernel API for create, lookup/open, close, and inspect.
- ✅ `objtest` self-test exercises multiple types, rights, duplication, invalid
  handles, and final-reference destruction; automated QEMU coverage added.
- ✅ Ownership and single-CPU synchronization assumptions documented; core
  object operations now guard against preemption (M7).

**M5 exit gate:** a kernel self-test creates and looks up multiple object types,
duplicates and closes handles, rejects invalid handles and rights, and proves
objects are destroyed exactly when their final reference is released. Document
handle ownership and the synchronization assumptions before introducing
concurrent callers in M7. M7 adds single-CPU preemption guards; SMP locks are
still future work.

## M6 — Capabilities ✅

- ✅ Capability objects bind a target object to an explicit rights subset.
- ✅ Derivation only reduces rights; revoking a parent invalidates its subtree.
- ✅ `capability_resolve()` checks revocation, requested rights and target type.
- ✅ `captest` rejects rights enlargement and verifies subtree revocation.
- ✅ Handle slots retire at generation exhaustion instead of wrapping and
  making an old stale handle valid again.
- ✅ A memory-range CONTROL capability gates page-permission changes; `captest`
  checks absent, insufficient, wrong-type, out-of-range, successful, and
  revoked authority. Mapping inspection now reports NX permissions as well.
- ✅ Memory-protection control is gated by a memory-range capability. New
  privileged service APIs must use capability checks and least-privilege
  defaults as they are introduced.
- ✅ Reduced-rights capability transfer over channels passed in QEMU as part
  of the M8 `capxfer` exit gate.

**M6 exit gate (passed):** `captest` proves rights cannot be enlarged by
derivation, revoking a parent invalidates its descendants, and the protected
memory operation rejects missing, insufficient, wrong-type, out-of-range, and
revoked capabilities. M8 later verified reduced-rights capability transfer
between handle tables with `capxfer`.

## M7 — Threads and scheduler ✅

- ✅ Single-CPU kernel threads with private guarded, NX stacks.
- ✅ Full interrupt-frame context switching and cooperative round-robin yield.
- ✅ Thread return and deferred stack reclamation.
- ✅ PIT-driven preemption and scheduling from the interrupt path.
- ✅ Single-CPU preemption guards for scheduler, PMM, VMM, heap, objects, and
  capabilities.
- ⏸ SMP-safe locks and interrupt routing (later SMP work).
- ⏸ User threads, per-process address spaces, and per-CPU run queues (M9+).

`threadtest`, `objtest`, and `captest` all passed in QEMU. The thread test
includes PIT preemption and dead-stack reclamation. Single-CPU interrupt
guards protect scheduler and shared allocator/object state; they do not
provide SMP safety. M7's single-CPU kernel-thread exit gate is passed; SMP
and user-space scheduling remain future work.

## M8 — IPC and channels ✅

- ✅ Bounded, non-blocking FIFO channels are rights-checked kernel objects;
  `chantest` passed in QEMU.
- ✅ Blocking receive and scheduler wake-ups; `chanwaittest` passed in QEMU.
- ✅ Reduced-rights capability transfer between kernel handle tables;
  `capxfer` passed in QEMU.
- ✅ PIT-tick receive timeouts; `chantimeout` passed in QEMU.
- ⏭ Cross-process message passing builds on M9 process and address-space support.

**M8 exit gate (passed):** `chantest`, `chanwaittest`, `capxfer`, and
`chantimeout` all passed in QEMU. The kernel now has bounded channels,
blocking receive, close and send wakeups, reduced-rights capability transfer,
and timeout support. User-process IPC depends on M9.

## M9 — User space 🚧

- 🚧 Private lower-half address spaces with shared supervisor-only kernel
  mappings; `asptest` is ready for QEMU testing.
- ⛔ Ring-3 execution and user/kernel transition stacks.
- ⛔ ELF loader for native Helix executables.
- ⛔ System-call entry (`syscall`/`sysret`) and the native ABI.
- ⛔ A minimal native libc-style runtime.
- ⛔ `init` as the first user-space process.

## M10 — Driver framework and isolated drivers

- Native driver interface and driver registration.
- User-space driver services.
- A working driver for at least one real device (e.g. a storage device via
  virtio), proving isolation.
- Device objects and `Driver.Control` capabilities.

## M11 — Storage and filesystems

- Block-device objects.
- A native Helix filesystem designed around objects, not inodes-as-fds.
- A read-only boot filesystem, then a writable one.

## M12 — Networking and graphics

- Network-interface objects, a native network stack.
- Display-surface objects and a compositor.
- Input-device objects.

---

## Compatibility track (much later)

These sit **above** the kernel and translate Unix behaviour into Helix's native
object system. They are not allowed to distort the native design.

- **ELF loader** for Linux/BSD binaries (distinct from the native loader in M9).
- **POSIX layer**: file descriptors, `fork`/`exec`, signals, mapped onto
  handles, objects and channels.
- **Linux runtime**: a Linux syscall translation layer.
- **BSD runtime**: a BSD syscall translation layer.

---

## Non-goals (for now)

- Linux or BSD driver compatibility. A clean native driver interface comes
  first.
- SMP on every milestone. Get one CPU rock-solid before adding more.
- Being fast. Be correct, then be fast.
- Security-hardening details before there is anything to secure.

## Deferred application experiment: Doom

Doom is deliberately outside the current kernel milestones. Keep the work
already done as boot-data and device-interface groundwork; do not link the
game engine into the kernel. Revisit a playable port after M9 establishes
userland, ELF loading, syscalls, and a native runtime. At that point, run Doom
as a userspace program and add only the runtime services it needs.

| Groundwork | State |
|------------|-------|
| DoomGeneric source imported (GPL-2.0-or-later) | ✅ upstream revision pinned |
| FreeDoom Phase 1 data and BSD-3-Clause notice | ✅ 0.13.0, local WAD or verified download |
| IWAD passed to kernel as a Limine module | ✅ recognized at boot |
| Framebuffer presentation and PS/2 key events | ✅ reusable platform groundwork |
| Userspace Doom runtime, renderer/input/timing adapter | ⏸ deferred until after M9 |
| Playable Doom launch from a userspace program | ⏸ deferred until runtime services are ready |

See [docs/07-doom-port.md](docs/07-doom-port.md) for implementation boundaries
and licensing details.
