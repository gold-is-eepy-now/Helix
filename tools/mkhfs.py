#!/usr/bin/env python3
"""Create a new, disposable HFS v1 image file."""

import argparse
import os
import struct
import zlib

SECTOR_SIZE = 512
HEADER = struct.Struct("<8sIIQQIIII")
MAGIC = b"HELIXFS1"
ROOT_START = 1
ROOT_SECTORS = 16
DATA_START = ROOT_START + ROOT_SECTORS


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", help="new image file path (must not exist)")
    parser.add_argument("--size-mib", type=int, default=64,
                        help="image size in MiB (default: 64)")
    parser.add_argument("--start-sector", type=int, default=0,
                        help="format a volume at this sector in an existing image")
    parser.add_argument("--volume-sectors", type=int,
                        help="volume length in sectors (required with --start-sector)")
    args = parser.parse_args()
    if args.size_mib < 1:
        parser.error("--size-mib must be at least 1")

    in_place = args.start_sector != 0 or args.volume_sectors is not None
    if in_place and (args.start_sector <= 0 or args.volume_sectors is None):
        parser.error("--start-sector and --volume-sectors must be used together")
    total_bytes = args.size_mib * 1024 * 1024
    total_sectors = (args.volume_sectors if in_place else
                     total_bytes // SECTOR_SIZE)
    if total_sectors <= DATA_START:
        parser.error("image is too small for HFS metadata")

    header = bytearray(SECTOR_SIZE)
    HEADER.pack_into(header, 0, MAGIC, 1, SECTOR_SIZE, total_sectors,
                     ROOT_START, ROOT_SECTORS, 64, 0, 0)
    checksum = zlib.crc32(header[:44]) & 0xFFFFFFFF
    struct.pack_into("<I", header, 44, checksum)

    # The default path is safe for users: exclusive create will never replace
    # existing data. In-place formatting is reserved for the image builder.
    flags = os.O_RDWR if in_place else os.O_WRONLY | os.O_CREAT | os.O_EXCL
    fd = os.open(args.image, flags, 0o644)
    if in_place:
        image_sectors = os.fstat(fd).st_size // SECTOR_SIZE
        if image_sectors * SECTOR_SIZE != os.fstat(fd).st_size or \
                args.start_sector + total_sectors > image_sectors:
            os.close(fd)
            parser.error("volume range is outside the existing sector-aligned image")
    try:
        if not in_place:
            os.ftruncate(fd, total_sectors * SECTOR_SIZE)
        offset = args.start_sector * SECTOR_SIZE
        os.pwrite(fd, header, offset)
        os.pwrite(fd, bytes(ROOT_SECTORS * SECTOR_SIZE),
                  offset + SECTOR_SIZE)
        os.fsync(fd)
    except BaseException:
        os.close(fd)
        if not in_place:
            os.unlink(args.image)
        raise
    else:
        os.close(fd)

    action = "HFS v1 volume initialized" if in_place else "HFS v1 image created"
    print(f"{action}: {args.image} ({total_sectors} sectors)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
