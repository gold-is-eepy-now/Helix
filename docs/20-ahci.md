# 20. AHCI (SATA): stage 1, read-only disk access

Status: **read-only; QEMU-tested only.** Not run on the iMac 11,2 or the MacBook Air 7,2.
No write path exists. The installer does not use it yet.

## What stage 1 does

- Finds the first AHCI controller by PCI class `01/06/01` (SATA, AHCI interface).
- Enables memory space and bus mastering in PCI command config, then maps the
  ABAR (BAR5, 32-bit memory BAR) uncached into a fixed virtual window
  (`AHCI_MMIO_VIRT`, six 4 KiB pages).
- Sets GHC.AE (AHCI enable), then for each implemented port (PI register):
  stops the port, allocates a command list, received-FIS area and command table
  from the physical frame allocator, starts the port again.
- Sends ATA `IDENTIFY DEVICE` (0xEC) to each port that has a disk, to read the
  model string and sector count.
- Registers each disk as a block device named `sata0`, `sata1`, ... through the
  generic `block_device_t` interface. Reads use `READ DMA EXT` (0x25), in chunks of
  8 sectors.
- Writes **nothing** to the disk. The write hook performs no transfer and returns 0.

The `disk` shell command lists the disks and reads sector 0, printing its boot
signature (`55aa` on a partitioned disk). Boot logs one `[ahci]` line.

## Why it runs after `vmm_init`

`vmm_map` walks the kernel page tables through `g_pml4`, which is set only by
`vmm_init`. Calling `ahci_init` earlier in `kmain` faulted in `next_table` (CR2 =
0xA00, the NULL pointer plus an offset) and the machine reset in a loop. The call
now sits directly after `vmm_init(hhdm_offset)` in `kernel/src/main.c`.

## Sources for the register layout

AHCI 1.3.1 specification, sections 3 (HBA registers), 4 (port registers) and 5
(FIS and command structures). The ATA command codes come from ATA/ATAPI-8 ACS.

| Item | Offset or value |
|---|---|
| HBA CAP / GHC / PI | 0x00 / 0x04 / 0x0C |
| GHC.AE | bit 31 |
| Port registers | 0x100 + 0x80 × n |
| Port CLB / FB | 0x00 / 0x08 (high halves at 0x04 / 0x0C) |
| Port IS / CMD / TFD / SIG / SSTS / SERR / CI | 0x10 / 0x18 / 0x20 / 0x24 / 0x28 / 0x30 / 0x38 |
| CMD.ST / FRE / FR / CR | bits 0 / 4 / 14 / 15 |
| IS.TFES (task-file error) | bit 30 |
| Command header | 32 bytes; command table PRDT at +0x80 |

## Limits

- Polled, one command slot (slot 0). No NCQ, no interrupts.
- No partition table parsing: the `disk` command only checks the signature of
  sector 0.
- Not tested on real hardware, including the iMac 11,2 and the MacBook Air 7,2.
- Writes, FLUSH and error recovery beyond a single retry-free command are not implemented.
