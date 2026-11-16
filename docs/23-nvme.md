# 23. NVMe: polled driver, reads, writes and flush

Status: **QEMU-tested only** (QEMU's emulated NVMe controller under TCG). Not run on real hardware.

Written from the NVM Express Base Specification: controller registers, the queue and phase-bit model,
Identify, Create I/O Completion/Submission Queue, and the NVM Read, Write and Flush commands. No other
kernel's source was used.

## What it does

- Finds the first controller by PCI class `01/08/02`, enables memory space and bus mastering, maps BAR0
  (32- or 64-bit; four pages, which covers the doorbells for queues 0 and 1).
- Checks CAP: at least 64 queue entries, 4 KiB minimum page size, NVM command set supported.
- Disables the controller, builds the admin queue pair (64 entries), programs AQA/ASQ/ACQ and CC
  (64-byte submission entries, 16-byte completion entries), and waits for CSTS.RDY within CAP.TO.
- Identify Controller (model, serial, firmware, MDTS), then the first active namespace (CNS 2, falling back to
  namespace 1) and Identify Namespace (size and LBA format). Only formats without per-block metadata and with
  512 to 4096-byte blocks are accepted.
- Creates one I/O completion queue and one submission queue, registers `nvme0`, and reads, writes and flushes
  through a 64 KiB bounce buffer. Transfers over two pages use a PRP list. Completion is polled using the phase bit.

## Shell

`disk` lists the namespace and its sector 0 signature. `blktest <dev> <lba> [keep]` writes a known 4 KiB pattern,
flushes, reads it back and compares, then writes the original data back and verifies that too. With `keep` the
pattern stays. The restore makes the default form safe, but do not run it on a disk another writer is using.

## Not done

- Interrupts (MSI-X), more than one I/O queue, queue sizes above 64, SGL, namespace management, multiple namespaces,
  metadata or protection information, power states, error log pages, controller reset on failure.
- A timeout leaves the command's queue slot in an unknown state; there is no abort.
- Hardware: nothing here has talked to a real drive. The MacBook Air 7,2 uses Apple's own PCIe SSD interface,
  which this driver may not support.

## Tests

`tests/shell-test.py` attaches a copy of the FAT32 test volume as an NVMe namespace and checks: `disk` lists
`nvme0`, a directory listing and a 100000-byte file sum through `fat nvme0` match the host, `blktest` writes,
reads back and restores at LBA 100, `blktest ... keep` at LBA 69000 and then reads the host's image file to confirm
the pattern really reached the backing store, and that an LBA past the end is refused.
