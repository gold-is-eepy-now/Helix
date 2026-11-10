# Apple T2 security SoC (iBridge2)

## What this is

Apple's T2 chip is the security companion SoC found in Macs built around the
T2 generation (up to and including iBridge2,15 — the 2020 Intel MacBook Air
with Retina display, among others). It sits next to the main x86 CPU and owns
work the main CPU must not do on its own:

- a Secure Enclave Processor (SEP). The T2 SEP is an ARM Cortex-A7 (A10
  silicon design), and it is where symmetric secrets, tokens and asymmetric
  credentials live;
- disk-encryption key handling;
- the Apple Boot ROM's Secure Boot chain (manifest checks for `boot.efi`,
  kernelcache and, on Big Sur and later, `apfs.kext` and `AppleImage4`).

This document describes **only what Helix implements**. Nothing here talks to
the SEP, unwraps keys, or verifies Apple manifests.

## What Helix implements

Helix talks to the T2's **iBridge Box Controller Engine (BCE)** mailbox, the
same mailbox firmware uses to accept commands from the host. The work is
split into two files:

| File | Role |
|---|---|
| `kernel/src/t2_protocol.c` | the mailbox wire format: message packing, out-box writes, reply reads. No kernel dependencies, so it can be unit tested on the build host. |
| `kernel/src/t2.c` | the driver: PCI discovery, MMIO mapping, firmware handshake, device object and capability. |

### PCI discovery

The BCE mailbox is exposed as a PCI function with vendor `0x106b` (Apple) and
device `0x1801`. `t2_register()` installs a `driver_t` whose `init` callback
runs inside `driver_init_all()`, so the probe happens with the rest of the
driver framework. The probe uses PCI configuration space only:

- `pci_find_device(0x106b, 0x1801, ...)` locates the function;
- BAR4 is the mailbox register block, BAR2 is the DMA ring pair;
- memory space and bus mastering are enabled on the function **and** on
  function 0 of the same slot, because the mailbox engine is driven through
  that peer function.

### Mapping and handshake

Mapping MMIO needs page tables, which Helix builds in `vmm_init()` before the
kernel stack is entered. `t2_start()` therefore runs in `kernel_main_continue()`,
after the mapping exists and before the shell takes the keyboard:

```c
vmm_map(T2_MMIO_VIRT, page_base, VMM_PRESENT | VMM_WRITE | VMM_NX | T2_PTE_PCD)
```

`T2_MMIO_VIRT` is `0xFFFFB00000000000`, a dedicated virtual range so the
mailbox never collides with USB or framebuffer mappings.

The handshake is one message. Mailbox messages pack a 6-bit type above a
58-bit value:

```c
#define T2_MB_MSG(type, value) \
    (((uint64_t)(type) << 58) | ((uint64_t)(value) & UINT64_C(0x03FFFFFFFFFFFFFF)))
```

Helix sends `SET_FW_PROTOCOL_VERSION` (type `0x0c`) with protocol version
`0x20001`, then polls the reply counter (bits 23:20 of the counter register
at offset `0x108`) until the firmware answers or 200 ms pass. A reply is
accepted only when its type and value match the request exactly. The driver
spins with the `pause` instruction and checks `time_uptime_ms()` so a wedged
firmware cannot hang boot.

### Device object and capability

A healthy mailbox produces a device object (`device_create()`) and a
capability carrying `OBJECT_RIGHT_CONTROL`. The capability is derived through
the normal capability path: a source handle with `READ | DUPLICATE | INSPECT |
CONTROL` is created on the kernel handle table, a capability with only
`CONTROL` is derived from it, and the source handle is closed again. The
capability can therefore be revoked, and revoking it invalidates anything
derived from it.

### Shell command

`t2info` prints what the driver found:

```text
T2 security SoC: present
  mailbox function: vendor:device 106b:1801 at bus:device.function
  BAR4 (mailbox registers): 0x...
  BAR2 (DMA rings): 0x...
  firmware: ready, protocol 0x00020001
  device object: 0x...  Device.Control capability: 0x...
```

On a machine without a T2 the same command prints `T2 security SoC: not
present`. This is the normal result under QEMU, which has no T2 device.

## What Helix does not implement

None of the following is present, and no code in this repository pretends
otherwise:

- **SEP services.** No key generation, key wrapping, secure-storage reads or
  writes, token handling, or asymmetric-credential operations. The SEP's own
  mailbox command set is not implemented; Helix speaks only the BCE host
  mailbox handshake.
- **Disk-encryption integration.** Helix does not ask the T2 to unlock a
  volume and holds no FileVault keys.
- **Apple Secure Boot verification.** No `boot.efi` manifest parsing, no
  ECID checking, no kernelcache or `apfs.kext`/`AppleImage4` snapshot
  verification. Those checks belong to Apple firmware and Apple-signed
  binaries, not to a clean-room driver.
- **Real-hardware verification.** The driver has been exercised only in QEMU
  (where no T2 exists, so only the "not present" path is proven) and in the
  hosted unit test that covers the wire format. No T2-equipped Mac has been
  tested.
- **Isolated driver service.** The driver runs in the kernel, not in a
  user-space service.

Nothing proprietary is required or embedded: no Apple binaries, no blobs, no
firmware images. Only the freely published register layout and message
encoding are used.

## Clean-room note

The mailbox register offsets, the message encoding and the handshake sequence
were taken from freely available descriptions of the BCE mailbox, notably the
`t2linux` `apple-bce-drv` project (GPLv2), consulted as a specification. No
code was copied from it; `kernel/src/t2_protocol.c` is written from scratch
against the documented behaviour.

## Testing

- `tests/t2-unit.c` drives `kernel/src/t2_protocol.c` on the build host with a
  fake register file: message packing, out-box writes, reply-counter decoding,
  reply composition and handshake validation. It runs as part of `make test`.
- `tests/shell-test.py` checks that `t2info` answers in the QEMU boot and
  reports the T2 state.
- The kernel-side path (PCI probe, MMIO mapping, handshake) is compiled and
  boots in QEMU, where no T2 is present, so only the "not present" branch is
  proven there.

## Next steps

Add SEP command support only if there is a concrete need for it, and only
against published protocol documentation. Until then, treat the T2 as a
detected device with a verified firmware handshake and nothing more.

## See also

- [docs/07-usb.md](07-usb.md) — the other hardware driver in the kernel, and
  the same MMIO mapping pattern.
- [docs/06-capabilities.md](06-capabilities.md) — capability derivation and
  revocation semantics used by the T2 control capability.
- [docs/05-object-manager.md](05-object-manager.md) — the object and handle
  model the T2 device object plugs into.
