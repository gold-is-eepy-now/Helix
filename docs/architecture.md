# Helix OS — Target Architecture

> This document describes the **intended** architecture. Almost none of it is
> implemented yet. Milestone 1 (boot + console) is done; everything below is
> the design we are building toward. Where something is implemented, it is
> marked ✅.

---

## The big picture

```
┌──────────────────────────────────────────────────────────────────┐
│  User space                                                       │
│                                                                   │
│   Native apps        Linux runtime      BSD runtime               │
│   (native API)       (POSIX→objects)    (POSIX→objects)           │
│                                                                   │
│   Driver services    System services                              │
│   (isolated)         (fs, net, display)                           │
└───────────────▲───────────────────────────────▲──────────────────┘
                │ channels / messages           │ syscalls (native ABI)
┌───────────────┴───────────────────────────────┴──────────────────┐
│  Kernel                                                           │
│                                                                   │
│   Object manager   Capability manager   IPC / channels            │
│   Scheduler        Virtual memory       Interrupts / timers       │
│   Physical memory  Driver interface     Native object API         │
└───────────────────────────────────────────────────────────────────┘
```

The kernel is **hybrid**: mechanisms that must be fast or privileged live
inside it (scheduling, VM, IPC fast paths, object and capability bookkeeping);
policy and device logic live in user-space services.

---

## Object model

Everything is an object. An object has:

- an **id** (opaque, kernel-managed),
- a **type** (process, thread, memory region, channel, display surface,
  network interface, storage volume, device, ...),
- a **lifetime** governed by reference counting,
- a set of **operations** valid for its type.

Planned object types:

| Object | Meaning |
|---|---|
| `Process` | An address space and a set of handles. |
| `Thread` | A schedulable execution context within a process. |
| `MemoryRegion` | A region of virtual memory with rights. |
| `Channel` | A bidirectional message endpoint. |
| `DisplaySurface` | A drawable framebuffer. |
| `NetworkInterface` | A network device. |
| `StorageVolume` | A block device / volume. |
| `Device` | A generic device object. |

**Status:** not implemented. First lands in milestone M5.

## Handles

Applications do not receive raw objects or Unix file descriptors. They receive
**handles**: small integers, meaningful only within the owning process's handle
table, that reference an object plus a set of rights. A handle cannot be forged
or used by another process except by explicit transfer.

## Capabilities

Authority is explicit and least-privilege by default:

- `Display.Draw`
- `Network.Connect`
- `Storage.Read`
- `Storage.Write`
- `Driver.Control`

A capability is itself an object. Capabilities can be:

- **derived** — create a weaker capability from a stronger one,
- **transferred** — sent through a channel to another process,
- **revoked** — invalidated so existing holders lose access.

The kernel enforces capabilities at every operation boundary. There is no
ambient "root".

**Status:** capability objects and revocation are implemented (M6); kernel
services are still migrating to capability checks.

## IPC: channels and messages

Processes communicate by sending messages through channel objects. This is the
native mechanism for:

- request/response between clients and services,
- transferring handles and capabilities,
- driver I/O.

**Status:** bounded channels, blocking receive and reduced-rights capability
transfer are implemented in M8. Cross-process messaging awaits user space.

## Drivers

Drivers are isolated user-space services wherever practical. A driver:

- runs in its own address space,
- holds only the capabilities it needs,
- exposes its device through a device object,
- talks to clients over channels.

The kernel provides a **native driver interface**. Linux/BSD driver
compatibility is explicitly out of scope until the native interface is mature.

**Status:** not implemented. Planned for M10.

## Memory

- Physical frames managed by a bitmap allocator.
- x86_64 page tables owned by the kernel, with a higher-half direct map of
  physical memory.
- Kernel heap for dynamic allocation.
- Per-process virtual address spaces, guard pages, non-executable stacks.

**Status:** not implemented. Planned for M2–M3.

## Scheduling and interrupts

- IDT with readable exception dumps.
- Interrupt controllers and a monotonic timer.
- Kernel threads, then user threads.
- A simple, fair scheduler; SMP later.

**Status:** not implemented. Planned for M4 and M7.

## The native object API and system calls

User space reaches the kernel through a native system-call ABI built around
objects and handles — not `open`/`read`/`write`. An ELF loader for native
executables and a minimal native runtime follow.

**Status:** M9 has started with private lower-half page-table roots and shared
supervisor-only kernel mappings. Ring-3 execution, the loader, and syscalls
remain to be implemented.

## Compatibility layers

Above the native API, runtimes translate Unix semantics into Helix objects:

- **POSIX layer** — file descriptors, `fork`/`exec`, signals.
- **Linux runtime** — Linux syscall translation.
- **BSD runtime** — BSD syscall translation.

These are guests. They must not distort the native design.

**Status:** not implemented. Far future (see ROADMAP).

---

## What exists today (Milestone 1)

✅ UEFI boot through Limine.
✅ x86_64 kernel entry in long mode.
✅ Serial console and framebuffer text console.
✅ Safe halt loop.

There is no memory manager, no scheduler, no IPC, no objects, no
capabilities, no drivers, and no user space. The architecture above is the
destination, not the current state.
