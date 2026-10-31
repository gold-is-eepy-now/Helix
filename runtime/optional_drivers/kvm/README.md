# KVM/QEMU/Virtio Driver Support

This directory contains optional driver support for KVM-based virtualization
and QEMU/virtio devices. This is an OPTIONAL subsystem — Helix boots and runs
cleanly without it.

## Purpose

Provide driver support for:
- **KVM**: Kernel-based Virtual Machine virtualization extension
- **QEMU**: Quick Emulator device emulation
- **Virtio**: Virtual I/O para-virtualized drivers for QEMU/KVM guests

## Components

| Component | File | Purpose |
|-----------|------|---------|
| KVM driver | `kvm_driver.c` | KVM virtualization support (VCPU management, memory slots, I/O) |
| Virtio block | `virtio_block.c` | Virtio block device support for QEMU disks |
| Virtio net | `virtio_net.c` | Virtio networking device support |
| Virtio console | `virtio_console.c` | Virtio console device support |
| Virtio ballon | `virtio_balloon.c` | Virtio memory balloon device |

## Building

This subsystem is compiled as part of the optional drivers library:

```sh
make linux-submodule
make linux-drivers
```

## Notes

- These drivers are for Helix running AS A GUEST under QEMU/KVM.
- Helix itself is not a hypervisor — it does not run other operating systems.
- The KVM driver here provides guest-side KVM interfaces (e.g., `/dev/kvm`)
  for userspace programs that want to run VMs inside Helix.
- This is not a Linux kernel fork. Driver code is either:
  - Adapted from Linux virtio sources via the transport adapter layer,
  - Or written specifically for Helix's driver framework.

## Current Status

KVM/virtio support is a skeleton framework. Actual driver implementations
require:
1. QEMU/KVM hardware emulation testing
2. Virtio specification compliance
3. Helix-native virtqueue and device model integration
