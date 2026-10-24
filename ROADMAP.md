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
- Up/Down command history (16 recent commands, retained for the current boot).
- Helix kernel-shell prompt: `helix:/#`; a `user@helix:~$` prompt waits for
  actual user identities and a user-space shell.
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
user space, ELF loading, real filesystems, persistent history, and completion.

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

## M9 — User space ✅

**M9 exit gate:** a boot-started `.hlx` user shell, static ELF loading into a
private address space, validated native syscalls, contained user faults, and
reclaim of the app's user and guarded kernel stacks on exit. M9 supports one
synchronous user process at a time. Filesystem loading belongs to M11/HFS;
concurrent user scheduling and compatibility runtimes are later work.

- ✅ Private lower-half address spaces with shared supervisor-only kernel
  mappings; `asptest` passed in QEMU.
- ✅ Helix-owned GDT, user descriptors, and TSS privilege-transition stack;
  passed QEMU boot.
- ✅ `usermodetest` passed controlled ring-3 entry/return and contained user
  exception handling in QEMU.
- ✅ Restricted static x86-64 ELF `ET_EXEC` loader and `.hlx` boot-module
  handoff. The exit-only sample app ran in QEMU and returned status 42 through
  `runapp`.
- ✅ Native `syscall`/`sysret` entry for bounded console `write`, `read`,
  `clear`, and `exit`; passed in QEMU.
- ✅ Each launched app receives a guarded NX kernel transition stack that is
  reclaimed on exit/fault; passed in QEMU.
- ✅ Minimal freestanding C wrappers `helix_write()` and `helix_exit()`; sample
  app passed in QEMU.
- ✅ User exception containment returns ring-3 faults to the kernel shell;
  `usermodetest` confirmed exception 6 in QEMU.
- ✅ Bounded console `read`/`clear` and first interactive user-space shell;
  prompt, commands, clear, and exit passed in QEMU.
- ✅ User-shell `sysinfo`, `neofetch`, and `edit` commands call the existing
  kernel information panel and volatile scratch editor; passed in QEMU.
- ✅ Boot starts `shell.hlx` as the initial user process and falls back to the
  kernel maintenance shell after exit; both passed in QEMU.

The current `.hlx` payload is a native ELF64 executable with a Helix extension,
not a new executable encoding. The boot-provided user shell and its commands,
clear operation, exit, and boot fallback passed in QEMU. Native wrappers now
use `syscall`/`sysret`; that path passed in QEMU. The user process currently
runs synchronously; its guarded transition stack is allocated and freed
per launch and passed QEMU validation. M9's single-process exit gate is met.
Concurrent user scheduling and filesystem-
backed app discovery are later work; app discovery moves to HFS in M11. User
accounts and login are deferred until those foundations exist.

## M10 — Driver framework and isolated drivers

- ✅ Bounded kernel driver registry with device objects, per-device control
  capabilities, and a `drivers` inventory command. UART, PS/2 keyboard, and
  PIT are registered and reported active in QEMU.
- ✅ Native driver descriptors and start callbacks; registration and active
  state passed in QEMU.
- ✅ Capability-checked UART control and a process-scoped USB storage service
  channel are wired to the boot shell.
- ✅ Driver read, write, flush, and control callbacks now run outside the
  registry's interrupt-disabled critical section. The registry takes a short
  per-device in-flight lease around each callback; `driver_stop` refuses to
  stop a device until its active operations finish. Normal thread callers stay
  timer-preemptible during polled I/O.
- 🚧 PCI discovery and polled xHCI startup enumerate one connected device,
  configure USB Mass Storage Bulk-Only Transport, read SCSI capacity, and
  expose sector reads/writes through a kernel IPC worker. The physical 32 GB drive
  enumerates and reports 60,440,000 512-byte sectors; do not format it.
- 🚧 EHCI USB 2.0 support now has a polled async schedule, high-speed USB mass
  storage enumeration, BOT capacity/read/write operations, and shares the
  existing block-storage worker. QEMU EHCI passed enumeration, capacity
  discovery, service registration, and an LBA 0 read from a disposable FAT32
  image, including an ICH9 EHCI + UHCI companion topology. VirtualBox boots the
  ISO and starts EHCI (8086:265c). Real VBoxManage CLI testing with the
  6557:002b high-speed drive reproduced unavailable storage. Both a boot-time
  attach and a hot-attach after the shell started left the EHCI port connected
  but disabled (`PORTSC=0x1001`); VBox reported the proxy attached at HighSpeed.
  Retrying enumeration from the `usb` command did not recover it. The kernel
  reads EHCI per-port power capability and waits for reset completion and
  reconnection, but VirtualBox EHCI mass storage remains unresolved. Full/low-
  speed devices are handed to an OHCI companion, which Helix does not implement.
- ✅ Virtual QEMU sector reads returned distinct expected patterns for LBA 0
  and 1; an out-of-range LBA was rejected. The physical drive's LBA 0 also
  read successfully and returned a boot sector with the `XBOX360 ` OEM field.
- ✅ `drivercaptest` passes in QEMU: a ring-3 process without a USB service
  channel cannot obtain it or read with a forged handle.
- 🚧 Storage requests now cross bounded IPC to a persistent kernel worker with
  read/write capabilities. The USB transport callback remains kernel code;
  executing it as an isolated user-space driver is still future work.
- M10 remains in progress: USB transport callbacks and their IPC worker still
  execute in kernel context; isolated user-space drivers and concurrent user
  processes are not implemented. Robust disconnect/timeout recovery and
  multiple-device support also remain.
- ACPI discovery, interrupt routing, and firmware-assisted power/shutdown
  support are future platform work; no ACPI subsystem is implemented yet.

## M11 — Storage and HFS (Helix Filesystem)

- 🚧 FAT32 USB volume discovery and recursive directory listing: `usb ls usb0`.
  Userland `usb cat NAME.TXT` now reads root-level short 8.3 files up to 8 KiB.
  Tested on a disposable QEMU image with nested files.
- 🚧 Helix Edit Ctrl-S and `usb put NAME.TXT TEXT` create or replace a
  root-level `.TXT` file on FAT32 `usb0`. Writes use a fresh cluster chain,
  update the directory entry, free the old chain, and flush the device; this
  needs temporary free space for both versions. Short 8.3 names and files up to
  8 KiB are supported. `usb rm NAME.TXT` removes a root-level file and frees
  its FAT chain. QEMU write/readback/delete checks are available against a
  disposable FAT32 image. Loading files into Helix Edit, subdirectory
  saves/deletes, and long names remain.
- 🚧 Block-device objects now report sector geometry. USB mass storage and a
  single polled AHCI SATA disk use the same `block-storage` driver class; HFS
  and FAT32 scan registered devices. A mount namespace and explicit selection
  among several volumes are still missing.
- 🚧 **HFS (Helix Filesystem)** v1 has a checksummed superblock, fixed root
  directory, contiguous extents, and a host formatter (`make mkhfs`). It scans
  registered USB/SATA block devices for a valid HFS volume, validates root
  records and file extents, and checks file CRCs on reads. Native HFS path
  syscalls support root-file read, write/replace, delete, list, and cat. Helix
  Edit can open and overwrite an HFS root file. A disposable HFS image can be
  attached with `USB_IMAGE=build/hfs-test.img make run`. The shell has
  `hfs format DEVICE` for blank raw disks; it writes
  a whole-device HFS volume and only probes LBA 0, LBA 1, and the final LBA for
  zeroes, so only use it on a newly created disposable VDI.
- M11 HFS work remaining: subdirectories, stronger crash consistency and
  journaling, a filesystem-independent VFS/mount namespace, multiple-volume
  selection, and FAT32 load/overwrite/delete support.
- ✅ A UEFI installer ISO and TUI are implemented ahead of schedule. The user
  reports the updated installer works. The ISO
  booted in QEMU and installed the bundled GPT/ESP/HFS disk image to a fresh
  qcow2 USB target; spot read-back checks passed. Installation now requires an
  exact device name plus confirmation, can replace an existing HFS volume, and
  relocates backup GPT metadata to the end of larger disks. SATA installation
  replaces partition metadata and data in the first 64 MiB; use only a
  disposable target disk. Broader hardware validation remains future work.
- AHCI currently supports one SATA disk with 512-byte logical sectors and
  32-bit or 64-bit DMA; VirtualBox VDI detection remains to be confirmed. NVMe,
  other AHCI disks, removable SATA hotplug, and physical-drive validation remain.
  The attached physical USB disk was not modified during development.
- The HFS path syscalls and editor loading/overwriting are implemented. Persistent
  shell history and explicit multi-device mount selection remain.
- Rescue-shell cleanup is underway: Helix Edit is now available only through
  userland. The rescue shell keeps diagnostics and app recovery/launch support.
  The user shell now includes `calc` (signed 32-bit `+ - * /`) and `about` as a
  system-info alias. Next userland apps: text viewer, file manager, and app
  launcher backed by filesystem paths.
- FAT32 USB support currently provides recursive short-name listing, root-file
  reads up to 8 KiB, `ls`/`cd`/`pwd` navigation by short 8.3 directory name,
  and creation of new root-level `.TXT` files. Next: overwrite and delete
  safely, support long filenames, and improve volume selection. USB host and
  storage support will be rebuilt around a user-space
  driver service as M10 continues.
- User profiles should follow a login shell, HFS subdirectories, and permission
  checks. The current `user@helix` prompt is a single fixed identity and does
  not provide authentication or account isolation.

## M12 — Networking and graphics

- Network-interface objects, a native network stack.
- Display-surface objects and a compositor.
- Input-device objects.
- First desktop experiments can begin once surface and input drivers work:
  start with a simple graphical shell, window, and app launcher.

## Later — Installer completion

The initial live-image ISO and TUI installer work on the user's setup. Broader
hardware validation and polished recovery paths remain. The live environment
remains usable without installing Helix.

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
