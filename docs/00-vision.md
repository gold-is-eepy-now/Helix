# Helix OS — Vision

## The problem with starting from Unix

Almost every hobby OS — and many production ones — begins by reimplementing
Unix: file descriptors, `open`/`read`/`write`, `fork`, signals, a global
namespace, and a single root user. Those ideas are fifty years old and carry a
lot of accidental complexity. In particular, the Unix model makes authority
implicit: a process that can open a file can usually open *any* file it has
permission to, and "root" is an all-or-nothing escape hatch.

Helix starts somewhere else.

## The native model

### Everything is an object

There is no special class of "file". A process, a thread, a memory region, a
display surface, a network interface, a storage volume, a channel, and a device
are all **objects**. An object has a type, a lifetime, and a set of operations
that can be performed on it.

### Handles, not file descriptors

An application never sees a raw object. It receives a **handle**: a small,
opaque, unforgeable reference to an object with a specific set of rights. A
handle is scoped to the process that holds it. Closing a handle drops one
reference to the object.

### Capabilities, not root

Authority is explicit and grantable at a fine grain:

- `Display.Draw`
- `Network.Connect`
- `Storage.Read`
- `Storage.Write`
- `Driver.Control`

A program that needs to draw gets `Display.Draw` and nothing else. There is no
ambient authority. **Least privilege is the default**, not a hardening step
added later. Capabilities can be derived (with reduced rights), transferred
through channels, and revoked.

### Message passing

Objects communicate by sending **messages** through **channels**. A channel is
itself an object. This replaces the Unix pattern of shared global state and
ad-hoc pipes with an explicit, auditable data flow. Messages are how
capabilities are transferred between processes.

### Isolated driver services

Drivers are a leading cause of kernel crashes in monolithic systems. In Helix,
drivers are **user-space services** wherever practical: a driver runs in its own
address space, holds only the capabilities it needs, and communicates over
channels. If a driver crashes, the kernel survives. The kernel provides a clean
**native driver interface**; Linux/BSD driver compatibility is explicitly not a
near-term goal, because inheriting another system's driver ABI would drag its
assumptions into ours.

### Hybrid modular kernel

Helix is a hybrid: performance-critical mechanisms (scheduling, virtual memory,
IPC fast paths, the object manager) live in the kernel, while policy and device
logic live in services. "Hybrid" here means we choose per subsystem rather than
following an ideology.

### Native object API

Applications target the native object API first. This is the API we design for,
optimize for, and document. Compatibility is a translation layer, not a design
constraint.

## Compatibility, later

Linux and BSD binaries will eventually run in **runtimes** above the kernel.
Those runtimes translate Unix behaviour — file descriptors, `fork`, signals,
POSIX semantics — into Helix objects, handles, capabilities and channels. They
are guests. The kernel stays native.

## Why start so small

Because a real OS is enormous, and the only way to finish is to always have
something that boots. Milestone 1 is a few hundred lines: firmware hands control
to a bootloader, the bootloader hands control to our kernel, the kernel prints a
banner and stops. That tiny system proves the entire chain — toolchain, linker
script, boot protocol, memory layout, output — and becomes the foundation every
later milestone builds on without breaking.

If it does not boot, it is not progress.
