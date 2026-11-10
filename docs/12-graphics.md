# 12. Graphics

Status: **steps 1–3 built and QEMU-tested.** When virtio-gpu is present, the kernel copies the
GOP screen into a 1280x800 backing buffer, scans it out through virtio-gpu, and flushes only the
changed rectangle after each draw. Without virtio-gpu the GOP framebuffer is used as before.

## Steps

| Step | What | Status |
| --- | --- | --- |
| 1 | virtio 1.0 PCI transport (`virtio.c`): capabilities, 64-bit BARs, feature negotiation, split virtqueue; virtio-gpu `GET_DISPLAY_INFO` (`virtio_gpu.c`); shell command `gpu` | built, QEMU-tested |
| 2 | virtio-gpu 2D resource: `RESOURCE_CREATE_2D`, `RESOURCE_ATTACH_BACKING`, `SET_SCANOUT` | built, QEMU-tested |
| 3 | `TRANSFER_TO_HOST_2D` and `RESOURCE_FLUSH`: draw into a resource, flush the changed rectangle to the screen; a display interface above GOP and virtio | built, QEMU-tested (`fb.c` dirty rectangle, `fb_present`) |
| 4 | Desktop drawing through the new interface, with double buffering | not started |

## Step 1: how the transport works

- The device is found by PCI vendor `0x1AF4` and device `0x1050` (virtio-gpu).
- Vendor capabilities in PCI config space (capability list at offset `0x34`) name a BAR and
  an offset for each structure: common config (status, features, queue addresses), notify,
  and device config (`num_scanouts` at offset 8). BARs may be 64-bit (two dwords).
- Handshake (virtio 1.0 section 3.1.1): reset, ACKNOWLEDGE, DRIVER, read features, accept only
  `VIRTIO_F_VERSION_1`, FEATURES_OK (read it back), set up queues, DRIVER_OK.
- A split virtqueue is three areas in one physically contiguous region: descriptors, the avail
  ring (we post requests here), and the used ring (the device reports completions here).
- One request is outstanding at a time. A request is two descriptors: the command (device
  reads it) and the response (device writes it). We kick the device by writing the queue
  index to the notify address, then poll the used index.
- `GET_DISPLAY_INFO` returns a header and one rectangle per scanout. Step 1 reads them and logs
  them; it does not change the screen.

## Tests

- The shell suite starts QEMU with `-device virtio-gpu-pci` and checks that `gpu` reports
  `gpu: scanout 0: ` (the suite confirms the device answers).
- Checked by hand under QEMU: `gpu` reports `scanout 0: 1280x800`.
- Not tested: real hardware, other virtio-gpu feature bits, error responses from the device.

## Limits of step 1

- One request at a time, polled, with a two-second timeout. No interrupts.
- Only the control queue is set up. The cursor queue is not used yet.
- Drawing goes through virtio only when virtio-gpu is present and the GOP format is 32-bit B8G8R8X8.
