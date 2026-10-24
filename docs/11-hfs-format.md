# HFS v1 disk format

HFS means **Helix Filesystem**. This document defines the first on-disk format
slice. The initial implementation is deliberately small: a raw 512-byte-sector
volume, a fixed root directory, and contiguous file extents. It is intended for
Helix-managed disk images while the storage and filesystem APIs are still
developing.

The host `tools/mkhfs.py` utility can create new regular image files in this
format; it refuses to overwrite an existing path. In a bootable Helix disk,
sector numbers in this format are relative to the HFS partition. The kernel
finds the HFS partition from the GPT and accesses it through the storage
driver's read/write capabilities. A raw HFS image uses sector zero as its
superblock.

## Layout

All integers are little-endian. Sector size is fixed at 512 bytes.

| Region | Sector range | Purpose |
| --- | --- | --- |
| Superblock | 0 | Format identity, geometry, feature flags and checksum |
| Root directory | 1–16 | 128 fixed 64-byte records |
| Data area | 17–end | File extents, one contiguous extent per file |

The superblock fields are:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 8 | ASCII `HELIXFS1` |
| 8 | 4 | Version (`1`) |
| 12 | 4 | Sector size (`512`) |
| 16 | 8 | Total sectors |
| 24 | 8 | Root directory start sector (`1`) |
| 32 | 4 | Root directory sectors (`16`) |
| 36 | 4 | Directory record size (`64`) |
| 40 | 4 | Feature flags (`0` in v1) |
| 44 | 4 | CRC-32/ISO-HDLC of bytes 0–43 |
| 48–511 | — | Reserved; must be zero when formatted |

Each root directory record has this layout:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 1 | State (`0` free, `1` regular file) |
| 1 | 1 | Name byte length (1–39) |
| 2 | 2 | Flags (zero in v1) |
| 4 | 8 | First data sector |
| 12 | 8 | File byte length |
| 20 | 4 | CRC-32/ISO-HDLC of file bytes |
| 24 | 40 | UTF-8 name bytes followed by zero padding |

Names are case-sensitive UTF-8 byte strings, 1–39 bytes, and cannot contain
`/`, NUL, or the names `.` and `..`. An empty file has length zero and first
data sector zero. Non-empty files occupy `ceil(length / 512)` consecutive data
sectors. Unused bytes in the last sector are zero. Extents must lie wholly in
the data area and must not overlap another active record. A deleted record is
zeroed, so its extent can be reused by a later write.

## Initial limitations

HFS v1 has one root directory, 128 entries, no subdirectories, and no
journaling. Implemented operations include list, CRC-checked read, create,
replace, and delete through HFS paths such as `hfs:/notes.txt`. A replacement
uses a separate free extent and publishes its directory record last, preserving
the old copy when a data write fails. This ordering does not provide full
power-loss atomicity. The kernel validates volume geometry, root records, file
names, checksums, and extents before exposing a mounted volume.

The host formatter creates new image files and can initialize the HFS region
inside an image under construction. The shell command `hfs format DEVICE`
creates a raw HFS v1 volume across a blank block device. It checks only LBAs 0,
1, and the final LBA for zeroes; use it only on a newly created disposable VDI
or USB image. It does not create a GPT partition table. The installer ISO
carries a prebuilt GPT/ESP/HFS image. The TUI requires an exact registered
device name and confirmation, replaces the target disk layout with that image,
and relocates the backup GPT header for larger disks. It overwrites partition
metadata and data in the first 64 MiB. AHCI support is limited to one
512-byte-sector disk and still needs runtime validation.
