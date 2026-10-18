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

**Still to do:** APIC/IOAPIC timers; per-process address spaces (needs the
object manager).

## M5 — The object manager ✅

- ✅ The first real Helix concept: a global object table.
- ✅ Object types with reference counting.
- ✅ Rights-bearing handles with stale-handle rejection and rights-reducing
  duplication; the current kernel handle table is ready to move into processes.
- ✅ A native kernel API for create, lookup/open, close, and inspect.
- ✅ `objtest` self-test exercises multiple types, rights, duplication, invalid
  handles, and final-reference destruction; automated QEMU coverage added.
- ✅ Ownership and single-CPU synchronization assumptions documented; process-
  owned handle tables and concurrent synchronization remain sequenced with M7.

**M5 exit gate:** a kernel self-test creates and looks up multiple object types,
duplicates and closes handles, rejects invalid handles and rights, and proves
objects are destroyed exactly when their final reference is released. Document
handle ownership and the synchronization assumptions before introducing
concurrent callers in M7.

## M6 — Capabilities 🚧

- ✅ Capability objects bind a target object to an explicit rights subset.
- ✅ Derivation only reduces rights; revoking a parent invalidates its subtree.
- ✅ `capability_resolve()` checks revocation, requested rights and target type.
- ✅ `captest` rejects rights enlargement and verifies subtree revocation.
- ⛔ Capability transfer over channels (M8).
- 🚧 Migrate privileged operations to require capabilities and establish
  least-privilege defaults as those operations are added.

**M6 exit gate:** self-tests must prove rights cannot be enlarged by
derivation, revoked capabilities and descendants cannot resolve their targets,
and kernel operations reject missing or insufficient capabilities. Channel
transfer remains sequenced with M8.

## M7 — Threads and scheduler

- Kernel threads, then user threads.
- Context switching.
- A simple round-robin or priority scheduler.
- Per-CPU run queues.

## M8 — IPC and channels

- Channels as first-class objects.
- Synchronous and asynchronous message passing.
- Blocking receive, timeouts, and wake-ups tied to the scheduler.

## M9 — User space

- ELF loader for native Helix executables.
- System-call entry (`syscall`/`sysret`), the native ABI.
- A minimal native libc-style runtime.
- `init` as the first user-space process.

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

## Doom experiment

| DoomGeneric source imported (GPL-2.0-or-later)                  | ✅ upstream revision pinned              |
| Optional IWAD passed as Limine module                           | ✅ `DOOM_WAD=/path/to/doom1.wad`          |
| Helix framebuffer + keyboard event primitives                   | ✅ groundwork                            |
| Doom runtime, WAD reader, input/timing glue                     | ⛔ in progress                            |
| Launch Doom from Helix shell and play under QEMU                | ⛔ not yet working                        |
