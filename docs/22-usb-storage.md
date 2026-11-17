# 22. USB mass storage: read-only

Status: **read-only; QEMU-tested only** (QEMU `usb-storage` on the UHCI companion layout, directly on a
root port and behind a hub). Not run with a real USB stick, and not on the iMac 11,2.

Written from the public USB Mass Storage Class Bulk-Only Transport 1.0 specification and the SCSI
block commands. No other kernel's source was used.

## Layers

1. **UHCI bulk transfers** (`usb_host.c`, `usb_host_bulk`): one transfer of up to 4096 bytes, one TD
   per packet on a bulk queue head linked after the control queue. IN TDs set short-packet-detect so a
   short packet ends the transfer. The DATA0/DATA1 toggle is tracked per endpoint direction. Result:
   0 ok, -2 stalled, -1 error or 3 s timeout.
2. **Bulk-Only Transport** (`usb_storage.c`): 31-byte CBW out, optional data phase, 13-byte CSW in,
   with tag and signature checks. A stalled data phase has its halt cleared and the CSW is still read.
   A bad or stalled CSW, a phase error or a transport failure triggers Bulk-Only Mass Storage Reset and
   clearing both halts.
3. **SCSI**: INQUIRY (vendor and product, device type must be a disk), TEST UNIT READY with REQUEST SENSE
   (up to 8 tries, 100 ms apart, because a new disk first reports unit attention), READ CAPACITY(10), and
   READ(10) in chunks of one bulk transfer. A failed read is retried once after REQUEST SENSE.
4. **Block device**: each disk registers as `usb0`, `usb1`, ... (up to 4). `disk` lists them with the
   sector 0 signature, and `fat usb0 ls /` reads a FAT32 stick (docs/21-fat32.md).

## Not done

- Writes, SYNCHRONIZE CACHE, READ CAPACITY(16) (disks over 2 TiB are refused with a message), multiple
  LUNs, UAS, and non-SCSI subclasses.
- Hot-plug: disks are found at boot only, because the USB stack enumerates only at boot.
- High-speed data: as the rest of the stack does, EHCI is reset and every device runs at full speed through
  the UHCI companions (64-byte bulk packets), so transfers are slow. 100 KB reads under TCG take seconds.
- Error injection: QEMU gives no easy way to make a device stall, so the reset and halt-clearing paths are
  written to the spec but have not been exercised by a test.
- Locking: bulk transfers share one set of buffers per controller, like control transfers. Do not call
  from two threads at once.

## Tests

`tests/usb-test.py` makes a 34 MiB FAT32 image with `mkfs.fat` and `mtools` (skipped with a note if they
are missing), attaches it as `usb-storage` and checks: the `[usb-storage] usb0:` boot line, `disk` listing
and the 55aa signature, `fat usb0 info`, `ls`, `cat`, and the byte sum of a 100000-byte file against the
host's sum. The second scenario repeats the key checks with the stick behind a hub.
