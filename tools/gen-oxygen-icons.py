#!/usr/bin/env python3
"""Packs the Oxygen 32x32 icons (assets/oxygen/32x32) into one file for the boot
loader to pass to the kernel as a module.

Usage: tools/gen-oxygen-icons.py OUT.pak

Pack format (all integers little-endian):
    "OXI1", u32 count
    count entries of 32 bytes: name[24] (NUL padded), u32 offset, u32 size
    payload: each icon as 32*32*4 bytes, B G R A per pixel, row-major, straight alpha
Offsets are from the start of the file. Requires Pillow."""
import os
import struct
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "assets", "oxygen", "32x32")
NAMES = ["computer", "help-about", "utilities-terminal",
         "accessories-text-editor", "internet-web-browser"]

def bgra(path):
    im = Image.open(path).convert("RGBA")
    if im.size != (32, 32):
        raise SystemExit("gen-oxygen-icons: %s is %dx%d, expected 32x32" % (path, im.size[0], im.size[1]))
    rgba = im.tobytes()                      # R G B A bytes, row-major
    out = bytearray(len(rgba))
    out[0::4] = rgba[2::4]                   # B
    out[1::4] = rgba[1::4]                   # G
    out[2::4] = rgba[0::4]                   # R
    out[3::4] = rgba[3::4]                   # A
    return bytes(out)

def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    payloads = [(n, bgra(os.path.join(SRC, n + ".png"))) for n in NAMES]
    table_len = 8 + 32 * len(payloads)
    table = b""
    data = b""
    for name, raw in payloads:
        table += name.encode("ascii").ljust(24, b"\0") + struct.pack("<II", table_len + len(data), len(raw))
        data += raw
    with open(sys.argv[1], "wb") as f:
        f.write(b"OXI1" + struct.pack("<I", len(payloads)) + table + data)
    print("gen-oxygen-icons: wrote %s (%d icons, %d bytes)" % (sys.argv[1], len(payloads), table_len + len(data)))

if __name__ == "__main__":
    main()
