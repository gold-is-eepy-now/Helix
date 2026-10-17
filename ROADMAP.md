# Helix OS — Roadmap

This roadmap is deliberately incremental. Each milestone is small enough to
finish, boot, and document. **Do not start a milestone before the previous one
boots and is documented.** The kernel does not become more real by writing more
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

## M3 — Kernel runtime foundations and memory management 🚧

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

**Still to do in M3:**

- ⛔ Guard pages and a non-executable stack.
- ⛔ A dynamically sized frame bitmap (currently a static 128 KiB array caps
  physical RAM at 4 GiB).

(Page tables, listed here originally, are done — see M4.)

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
- ✅ Test suite grown to 27 checks, including a deliberate fault caught and
  reported.

**Still to do:** guard pages and a non-executable stack; APIC/IOAPIC timers;
per-process address spaces (needs the object manager).

## M5 — The object manager

- The first real Helix concept: a global object table.
- Object types with reference counting.
- Handles: per-process tables mapping small integers to objects with rights.
- A native object API surface (create, open, close, inspect).

## M6 — Capabilities

- Capability objects that grant specific rights on specific objects.
- Capability derivation, transfer over channels, and revocation.
- Kernel enforcement points (every operation checks a capability).
- Least-privilege defaults.

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
