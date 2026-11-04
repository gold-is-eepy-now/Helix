# drivers/

Native Helix driver services (planned, milestone M10). Status: no user-space driver
service exists yet; the only block driver is the in-kernel ramdisk.

Drivers will be **isolated user-space services** wherever practical. Each driver
runs in its own address space, holds only the capabilities it needs
(`Driver.Control`, plus device-specific rights), exposes a device object, and
communicates with clients over channels.

The kernel exposes a clean **native driver interface**. Linux and BSD driver
compatibility is explicitly out of scope until that native interface is mature.

Nothing is implemented here yet. See [../ROADMAP.md](../ROADMAP.md).
