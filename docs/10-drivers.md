# M10: Driver framework

M10 begins with a kernel registry that represents each registered device as a
Helix `DEVICE` object. Registration assigns a unique bounded name and class,
keeps a device object alive through a rights-bearing handle, and issues a
revocable capability granting `CONTROL`. The kernel and user shell can inspect
the registry with `drivers`.

The initial records are the UART console, PS/2 keyboard, and PIT timer, which
are already initialized by the kernel. Their `drivers` inventory reported all
three as active in QEMU. The registry now accepts descriptors with start,
stop, and control callbacks, and routes control requests only after resolving
a `CONTROL` capability to the matching device object. Current core records
provide start callbacks that recognize their existing initialization; their
registration and active state passed in QEMU. The UART exposes one bounded
operation, `WRITE_BYTE`, through its control callback. The user-shell `uart X`
command reaches it through a syscall that validates the byte and requires the
per-process UART control handle. The boot shell receives a read-only handle to
the capability object; other `.hlx` apps receive no UART capability by default.
This validates mediated access to an actual device while keeping raw port I/O
inside the driver. `uart z` passed before this per-process handle change; the
updated per-process UART handle also passed the QEMU shell test. Core devices
do not support stop.

Registry access uses short interrupt-disabled critical sections. Read, write,
flush, and control operations acquire an in-flight lease, release the registry
guard, call the driver in the caller's interrupt state, then release the lease.
Normal thread callers stay timer-preemptible during polled storage I/O.
`driver_stop` refuses to stop a device while one of these operations is active,
avoiding a race between a stop callback and an active operation. The single-CPU
registry still does not provide SMP locks.

The project QEMU runner defaults to an emulated `qemu-xhci` PCI controller;
select EHCI USB 2.0 with `USB_CONTROLLER=ehci`. Set `USB_HOST_VID` and
`USB_HOST_PID`, or `USB_HOST_BUS` and `USB_HOST_ADDR`, to pass a selected host
USB device with `-device usb-host`; the runner disables guest reset for the
physical device. QEMU's USB host passthrough backend only works on supported
host platforms, so model availability in `-device help` does not guarantee
that macOS can pass a physical device through.

Helix scans PCI bus 0 for USB host controllers. For xHCI it maps the controller
BAR, reports capability registers and root-port status, starts/resets the
controller, and allocates DMA rings. For EHCI it maps the capability and
operational registers, hands controller ownership to the OS, starts the
asynchronous schedule, powers ports when the controller advertises per-port
power control, waits for reset completion/reconnection, resets high-speed
ports, and runs control and bulk transfers through DMA queue heads and
transfer descriptors. Full/low-speed
devices are routed to a companion controller, which Helix does not implement.
Both paths enumerate one Mass Storage Bulk-Only Transport device and use SCSI
READ CAPACITY for geometry. The boot shell receives bounded request/response
channel endpoints, and a persistent kernel worker holds storage `READ` and
`WRITE` capabilities to service sector requests over IPC. `usb read <LBA>`
displays the first 16 bytes of a sector. QEMU tests passed USB enumeration,
capacity discovery, service registration, and LBA 0 reads through both xHCI and
EHCI. The EHCI test used a disposable FAT32 image and returned its expected
boot-sector bytes. An emulated xHCI disk also returned distinct expected
patterns at LBAs 0 and 1, and rejected an out-of-range LBA. The physical 32 GB
drive enumerated, reported 60,440,000 512-byte sectors, and returned LBA 0
successfully. Its first bytes include the `XBOX360 ` OEM field, with BPB values
indicating 512-byte sectors and 32 sectors per cluster.

VirtualBox 7.2.20 was tested through VBoxManage using the EHCI controller and
the same 6557:002b high-speed drive. The latest ISO booted to the user shell;
VBoxManage attached the drive both before boot and after shell startup. In both
cases Helix saw a connected but disabled port (`PORTSC=0x1001`) and could not
enumerate storage. Running `usb` retries EHCI enumeration and registers the
storage service when a device becomes ready, but did not resolve this VBox
case. EHCI storage in VirtualBox remains an open issue; QEMU EHCI passes.

The user-mode `drivercaptest` proves a process without a granted channel cannot
obtain or forge storage service access. Sector operations cross IPC, but the
worker and USB transport still run in kernel context; an isolated user-space
driver service is not complete. The M9 runtime still launches one synchronous
ring-3 process at a time.

M11 now includes a bounded FAT32 backend for USB0. `usb ls usb0` recursively
lists short 8.3 names, and Helix Edit Ctrl-S creates a new root-level `.TXT`
file (up to 8 KiB) on FAT32 media. It refuses existing names; file loading,
overwriting, long names, subdirectory saves, robust power-loss handling, and
HDD/SSD discovery remain future work. Both recursive listing and editor
save/readback were tested on a disposable QEMU FAT32 image. The physical drive
was not used for write testing. FATX, including Xbox 360 volumes, is not
supported by this FAT32 backend. Current support remains single-device without
robust disconnect or timeout recovery. A virtio block device remains a simpler
fallback where host USB passthrough is unavailable.
