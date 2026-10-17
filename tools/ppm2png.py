#!/usr/bin/env python3
"""ppm2png.py — convert a binary PPM (P6) image to PNG.

QEMU's `screendump` monitor command writes PPM. We need PNG so the image can
be viewed and attached. This uses only the standard library (zlib + struct),
so it works in a minimal sandbox with no imaging packages installed.

Usage: python3 tools/ppm2png.py input.ppm output.png
"""
import struct
import sys
import zlib


def read_ppm_p6(path):
    with open(path, "rb") as handle:
        data = handle.read()

    # Parse the P6 header: magic, width, height, maxval. Tokens are separated
    # by whitespace and '#' starts a comment running to end of line.
    tokens = []
    index = 0
    while len(tokens) < 4:
        while index < len(data) and data[index] in b" \t\r\n":
            index += 1
        if index < len(data) and data[index] == ord("#"):
            while index < len(data) and data[index] not in b"\r\n":
                index += 1
            continue
        start = index
        while index < len(data) and data[index] not in b" \t\r\n":
            index += 1
        tokens.append(data[start:index])

    if tokens[0] != b"P6":
        raise ValueError("not a binary PPM (P6) file")
    width = int(tokens[1])
    height = int(tokens[2])
    maxval = int(tokens[3])
    if maxval != 255:
        raise ValueError("only 8-bit PPM files are supported")

    index += 1  # exactly one whitespace byte separates header from pixels
    pixels = data[index:index + width * height * 3]
    if len(pixels) != width * height * 3:
        raise ValueError("truncated pixel data")
    return width, height, pixels


def write_png(path, width, height, pixels):
    # Each scanline is prefixed with filter byte 0 (None).
    raw = bytearray()
    stride = width * 3
    for y in range(height):
        raw.append(0)
        raw += pixels[y * stride:(y + 1) * stride]

    def chunk(tag, payload):
        body = tag + payload
        return (struct.pack(">I", len(payload)) + body
                + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", ihdr)
           + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
           + chunk(b"IEND", b""))
    with open(path, "wb") as handle:
        handle.write(png)


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    width, height, pixels = read_ppm_p6(sys.argv[1])
    write_png(sys.argv[2], width, height, pixels)
    print(f"wrote {sys.argv[2]} ({width}x{height})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
