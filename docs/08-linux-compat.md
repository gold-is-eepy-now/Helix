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
