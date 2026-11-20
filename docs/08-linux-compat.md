# 08 — Linux driver compatibility layer

Goal: compile existing Linux driver source for Helix with minimal or no changes,
starting with simple devices and working toward GPUs, Wi-Fi, Ethernet, NVMe, SATA,
USB, Bluetooth and audio.

This document records what the code actually does today, the design built on top
of it, and what is done. Where a claim is not yet true it is marked as such.

## 1. Audit of the current kernel

Verified by reading the source in this repository (`kernel/src`, about 11,100
lines of C):

| Area | What exists | Blocker for Linux drivers |
| --- | --- | --- |
| Physical memory | Bitmap frame allocator (`pmm_alloc_frame`, `pmm_alloc_frames`) | No DMA-constrained or low-memory zones, no NUMA. No DMA address mapping API. |
| Virtual memory | Own 4-level page tables, per-process address spaces, `vmm_map`/`vmm_protect`. Physical memory is reached through the Limine higher-half direct map (`vmm_phys_to_virt`). | `ioremap` has to be written. Cache-control flags (uncached, write-combining) are not exposed. |
| Heap | First-fit `kmalloc`/`kfree`/`kzalloc` (`heap.c`), no locking | **Not safe under preemption.** The timer (IRQ0) calls `thread_tick`, so a thread can be switched out inside `kmalloc`. Two threads can corrupt the heap. Any driver that allocates from more than one thread needs a lock or an interrupt-off section here. |
| Interrupts | IDT with exception handlers. The PIC is remapped; the timer (IRQ0) is handled; other IRQs are acknowledged and dropped (`idt.c`). LAPIC only opens LINT0 for the 8259. | **No IRQ registration API.** No IOAPIC, no MSI/MSI-X. A PCI device with a legacy line cannot be served. |
| Locking | None. No spinlock, mutex, or atomic abstraction. | Every Linux driver assumes these. Built in LC-1 (section 4). |
| Scheduling | Round-robin kernel threads, saved context switches, timer preemption (`thread.c`). No sleep/wake primitive beyond `thread_block_current`. | Linux drivers sleep on wait queues. Need a wait queue and a timed sleep. |
| Timers | PIT at 100 Hz (`pit.c`), TSC-calibrated uptime (`time.c`). | No timer-list API (`timer_setup`, `mod_timer`), no hrtimers, no delayed work. |
| PCI | Config access via ports 0xCF8/0xCFC only; `pci_find_class` (`pci.c`). | No BAR sizing or mapping helpers, no bus-driver registration, no MSI capability, no PCIe extended config space (ECAM). |
| DMA | Ad hoc: `usb_host.c` takes frames from the PMM and uses 32-bit physical addresses. | No `dma_alloc_coherent`, no `dma_map_single`, no IOMMU. Every device is assumed to see all of RAM. |
| Device model | `driver_t` with init/probe/remove and object-based devices (`driver.c`). Only the ramdisk is registered as a block driver. | Not a Linux `device`/`driver`/`bus` model. No `kobject`, no sysfs, no hotplug. |
| Modules | None. Drivers are compiled into the kernel. | No loadable objects, no symbol table, no relocation. |
| Firmware | None. | No `request_firmware`. Many GPU and Wi-Fi drivers cannot start without it. |
| Networking | `net_interface.c` (66 lines): an interface object. No protocol stack. | No `net_device`, no `sk_buff`, no stack. |
| Graphics | `fb.c` (framebuffer from Limine), `display_surface.c` (41 lines). | No DRM/KMS, no GEM/TTM, no GPU memory management. |
| USB | UHCI host, hubs, HID keyboard and mouse (`usb_host.c`, `usb_hid.c`). Runs in polled mode from the input path. | No USB core API for class drivers, no URB model. |

Two claims in the earlier roadmap were wrong and are corrected in `ROADMAP.md`:
M10 said "user-space driver services" and "a virtio storage driver" were done; no
such code exists. M12 said there was a native network stack and a compositor; there
is neither.

## 2. Target: which Linux version

Helix targets **Linux 6.18 LTS**. Linux 6.18 is the current LTS branch, with
6.12 and 6.6 also maintained. The LTS choice means fewer API changes to chase.
Source: [6.18.40 LTS release report](https://www.warp2search.net/story/linux-kernel-61840-and-five-lts-branches-released-in-massive-july-stable-cycle/)
and [LTS branches overview](https://blog.lemaker.org/kernel-lts-sbcs-2026-6-18-vs-6-12-vs-6-6/).
Drivers are taken from that branch. Pin the exact version when the source is
imported, because LTS point releases change the code.

## 3. Options compared

**A. Native Linux API compatibility inside Helix (LinuxKPI-style).** Write the
`linux/*.h` headers that drivers include, and implement the functions they call on
top of Helix's own subsystems. This is what FreeBSD's LinuxKPI does. Drivers compile
against our headers, not the real Linux tree. It is the only option that keeps a
single kernel, and it is the one that fits the goal of native driver source.

**B. A hosted Linux kernel.** Run a whole Linux kernel as a guest, for example under
KVM with a passthrough device. This gives full driver support at once, but it makes
Helix a host for Linux, not a system that uses Linux drivers. It also needs
hypervisor work that does not exist.

**C. Linux Kernel Library (LKL, github.com/lkl/linux).** LKL builds the Linux kernel
as a library that a hosted userspace program links against. It needs a libc,
threads, timers, and a host-operations table (`lkl_host_ops`) supplied by the
embedder. It gives filesystem and network stacks (it runs them over disk and tap
backends), but **it has no path to PCI device access**. So it cannot drive a GPU or
a Wi-Fi card. It could later host an ext4/network stack for a userspace service, once
Helix has a libc and pthreads. It is not the driver path.

**D. Hybrid (chosen).** Option A is the main path: Linux driver source compiled
against compat headers, in the kernel. Option C is a possible later host for
filesystem and network-stack code in userspace, and option B is not planned. The
compat layer is built in stages so that every stage boots and is tested.

## 4. Architecture

```
  Linux driver source (unmodified where possible)
        │  #include <linux/...>
        ▼
  compat/include/linux/*.h     types, slab, spinlock, atomic, mutex, module, printk
        │
        ▼
  compat/src/*.c               glue: maps each Linux call onto a Helix subsystem
        │
        ▼
  Helix kernel subsystems      pmm/vmm/heap, idt/pic/apic, pci, thread/pit, object manager
```

Rules for the compat layer:

- Drivers see only `<linux/...>` headers. Helix's own headers stay out of driver
  code, so Linux names are never shadowed.
- A Linux function keeps its Linux signature. GFP flags are accepted even when they
  mean nothing yet, so drivers compile unchanged.
- Locking primitives are correct for Helix's single CPU and preemptive timer:
  spinlocks mask interrupts (section 1, "Locking").

## 5. Milestones

**LC-1 — compat headers, locking, and an initcall runner (in progress).**
- Headers: `types.h`, `errno.h`, `kernel.h` (printk, pr_*, BUG_ON), `slab.h`,
  `spinlock.h`, `atomic.h`, `mutex.h`, `module.h` (`module_init` into the
  `compat_initcalls` linker section), `init.h`.
- `compat/src/compat_core.c` runs every collected initcall at boot.
- `compat/src/hello_compat.c` is a self-test written only against the compat headers.
  It runs at boot and prints `[compat] self-test passed` when slab, spinlock with
  irqsave, atomic and printk all work.
- Not in LC-1: loading object files, real hardware, IRQ registration, DMA.

**LC-2 — loadable modules.** Load a relocatable `.ko` (ELF `ET_REL`): parse
sections, apply `R_X86_64_*` relocations, resolve undefined symbols against an export
table built from the compat layer (`EXPORT_SYMBOL`), call `init_module`. Test with a
small compiled module, and with the same module built from the Linux source tree.

**LC-3 — timers, wait queues, workqueues, IRQ registration.** `request_irq` on the
PIC first, then IOAPIC routing and MSI. Timed sleep on top of `pit`.

**LC-4 — PCI and DMA.** Full PCI config access (ECAM for extended space), BAR
mapping (`pci_iomap`, `ioremap`), `dma_alloc_coherent`, `dma_map_single`, and bus
mastering enable. An IOMMU is out of scope until later.

**LC-5 — first real driver: virtio.** A virtio-pci transport and virtio-blk or
virtio-net from Linux source, running under QEMU. This is the first end-to-end
proof of the compat layer on a real device.

**LC-6 — firmware loading, then more classes.** `request_firmware`, then USB
(URBs over the existing UHCI code), then storage (AHCI for SATA, NVMe), then
networking (`net_device` and a stack), then DRM/KMS for Intel and AMD GPUs.
NVIDIA and Wi-Fi come last: both depend on the most of the above.

Each milestone must build with the standard sequence and boot in QEMU before the
next one starts.

## 6. What is done (LC-1)

See the commit that adds `compat/` and `docs/08-linux-compat.md`. The boot log
prints the self-test result. The tests live in `tests/shell-test.py`.

## 7. Input and the applespi driver (QEMU-tested only)

Built on top of LC-1 without loading any `.ko`: applespi (GPL-2.0, from upstream
Linux, kept under `experiments/applespi/` as the reference and copied into
`compat/src/` for the build) compiles and links against the compat headers, and its
module registers at boot. Its probe path runs; it refuses to start on this machine.

**What was added**

- Headers (`compat/include/linux/`): `input.h` and `input/mt.h` (the key and
  multitouch subset), `spi/spi.h`, `acpi.h`, `efi.h`, `device.h`, `wait.h`,
  `workqueue.h`, `leds.h`, `debugfs.h`, `fs.h`, `string.h`, `delay.h`, `ktime.h`,
  `crc16.h`, `unaligned.h`, `compat_common.h` (bit ops, `ERR_PTR`, `guard()` and
  `scoped_guard()` cleanup scopes), and `asm/barrier.h`.
- `compat/src/input_shim.c`: a Linux input device on top of Helix's keyboard and
  mouse queues in `usb_hid.c` (`usb_hid_inject_key`, `usb_hid_inject_mouse`). Keys
  map to Helix key codes; Shift, Ctrl and Alt are announced once per pair. A touchpad
  moves the pointer by its first finger's travel divided by 16. The divisor is a guess,
  not calibrated.
- `compat/src/spi_shim.c`: SPI drivers register; `compat_spi_add_device()` offers a
  device to the driver whose ACPI id matches. `spi_async()` queues messages, and
  `compat_spi_pump()` runs each one through Helix's kernel SPI layer (`spi_transfer`,
  docs/17-spi.md). The slave is linked from ACPI (`compat/src/spi_acpi.c`): its `_CRS`
  SpiSerialBus gives the chip select, clock phase and polarity, and its parent's `_ADR` must
  equal a registered controller's ACPI address. Without that match the message fails with
  `-ENODEV`, and `spi` says why. Only the linked device's messages are accepted. A transfer
  that asks to hold chip select across a later transfer is refused with `-EOPNOTSUPP`.
  Controller errors map to `-ENODEV` (not ready), `-EINVAL` (bad argument) and `-EIO`
  (timeout or overrun).
- `compat/src/acpi_shim.c`: every ACPI call returns `AE_NOT_SUPPORTED`.
- `compat/src/misc_shim.c`: time from the TSC clock, busy-wait delays, CRC-16,
  `snprintf` and a minimal `sscanf`, leds (recorded, not visible), debugfs (accepted
  and discarded), a workqueue run by the SPI pump, and EFI with no runtime services.
- `compat/src/applespi_trace.h`: no-op stand-ins for the upstream tracepoints.
- `compat/src/input_selftest.c`: the `inputtest` shell command. It runs keyboard and
  touchpad reports through the shim, checks the Helix queues, and checks that the
  applespi probe is reached and returns `-ENODEV`. It also checks the SPI bridge: with no
  slave is linked a message fails with `-ENODEV`; then a fake controller, lent to the bridge
  through a test hook (it is not added to the kernel's controller table), checks completion,
  the reply bytes, chip select and mode, the `-EIO` timeout path and the `-EOPNOTSUPP`
  refusal. The real slave's binding is put back afterwards.
- The `spi` shell command prints the link for APP000D without sending anything: the parent
  it sits under, the parent's `_ADR`, the `_CRS` settings, and either "linked" or the reason
  it is not.
- Warning flags for `compat_applespi.o` only (`kernel/Makefile`): upstream's
  designated initialisers and unused callback arguments are silenced for that file.

**What is not true yet**

- applespi cannot run on the 2010-2017 MacBook it targets. It needs ACPI methods
  (`UIST`, `SIEN`, `SIST`, `_GPE`) and properties from the DSDT. Helix has table
  parsing (docs/16-acpi.md) and an AML interpreter (docs/24-aml.md), which answers
  `acpi_get_handle` and `acpi_evaluate_integer`. Stage 1 (done) binds the device: the
  compat layer finds the AML Device whose `_HID` is `APP000D` (`compat_acpi_find_hid`),
  stores its node in `struct device` (so `ACPI_HANDLE()` returns it), and the probe then
  runs `UIST`, finds `SIEN` and `SIST`, and reads `_GPE`. Stage 2 (done) installs and
  enables that GPE through the SCI handler (docs/16-acpi.md). The probe then registers the
  keyboard and returns success. The SPI bridge now exists (below), but on QEMU there is no
  controller that can talk to APP000D, so no key events arrive. That success means the
  ACPI steps passed, not that the keyboard works.
- The `_DSD` properties applespi reads (`spiCSDelay`, `resetA2RUsec`, `resetRecUsec`) are
  not implemented. The probe warns that they are missing and continues, which is allowed
  by the driver.
- No SPI transfer has run against the Apple device or against any real controller. The
  bridge has run only against the QEMU absent path (`-ENODEV`) and a fake controller in
  the kernel self-test, under QEMU TCG.
- Input events do not reach Helix's `input_device_t` objects; they land in
  `usb_hid`'s queues, which the shell and desktop already read.
- Only the keys in the `linux/input.h` enum map to Helix codes. Other keys are dropped.
- `msleep()` busy-waits; there is no sleeping in compat drivers yet.
- Tracepoints do nothing. `%*ph` in applespi's warnings is not parsed by Helix's
  formatter, so those lines print the raw spec.

**Next steps for this area:** bind APP000D to its SPI controller through the ACPI parent
link and read its chip select and speed from `_CRS` (the SpiSerialBus resource); parse the
`_DSD` properties; then a real transfer on the MacBook Air, which QEMU cannot provide.
The touchpad calibration follows.

**Test data (test builds only).** The suite loads a synthetic SSDT with QEMU's `-acpitable`:
`tests/gen-applespi-ssdt.py` writes a device `\_SB.APPT` with `_HID "APP000D"`, `_GPE` 16,
and the methods `UIST`, `SIST` and `SIEN`. It does not ship in the release image. A real
Apple DSDT is not available under QEMU, so this table only proves the bind and the method
calls, not the Apple firmware.

**Test:** `inputtest` in the shell suite (`tests/shell-test.py`). Its output on QEMU
(TCG, no /dev/kvm) is:

    inputtest: ok   KEY_A press types 'a'
    inputtest: ok   KEY_A with shift held types 'A'
    inputtest: ok   touchpad finger moving 160 units moves the pointer 10 px
    inputtest: ok   applespi refuses without ACPI: probe returns -ENODEV
    inputtest: ok   the AML namespace has an APP000D device to bind
    inputtest: ok   applespi's ACPI steps succeed (GPE installed and enabled, keyboard registered)
    inputtest: PASS (0 failure(s))
