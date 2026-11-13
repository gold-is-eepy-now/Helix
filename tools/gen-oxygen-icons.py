#!/usr/bin/env python3
"""Packs the Oxygen 32x32 icons (assets/oxygen/32x32) into one file for the boot
loader to pass to the kernel as a module.

Usage: tools/gen-oxygen-icons.py OUT.pak

Pack format (all integers little-endian):
    "OXI1", u32 count
    count entries of 32 bytes: name[24] (NUL padded), u32 offset, u32 size
    payload: each icon as 32*32*4 bytes, B G R A per pixel, row-major, straight alpha
Offsets are from the start of the file.

The PNG files are decoded here with the standard library only (zlib), so the build
needs no Pillow. Supported: RGBA, 8 or 16 bits per channel, non-interlaced, which is
what the Oxygen set uses. A 16-bit channel keeps its high byte, as Pillow does."""
import os
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "assets", "oxygen", "32x32")
NAMES = ["computer", "help-about", "utilities-terminal",
         "accessories-text-editor", "internet-web-browser"]

def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c

def read_png_rgba(path):
    """Return (width, height, RGBA bytes) for a non-interlaced 8/16-bit RGBA PNG."""
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit("gen-oxygen-icons: %s is not a PNG" % path)
    pos, idat = 8, []
    w = h = depth = ctype = interlace = None
    while pos < len(data):
        (ln,) = struct.unpack(">I", data[pos:pos + 4])
        typ = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + ln]
        pos += 12 + ln
        if typ == b"IHDR":
            w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
        elif typ == b"IDAT":
            idat.append(body)
        elif typ == b"IEND":
            break
    if ctype != 6 or interlace != 0 or depth not in (8, 16):
        raise SystemExit("gen-oxygen-icons: %s: only non-interlaced 8/16-bit RGBA PNGs are supported" % path)
    raw = zlib.decompress(b"".join(idat))
    bpp = 4 * (depth // 8)                  # bytes per pixel in the stored data
    stride = w * bpp
    out = bytearray()
    prev = bytearray(stride)
    p = 0
    for _ in range(h):
        f = raw[p]
        line = bytearray(raw[p + 1:p + 1 + stride])
        p += 1 + stride
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if f == 0:
                pass
            elif f == 1:
                line[x] = (line[x] + a) & 255
            elif f == 2:
                line[x] = (line[x] + b) & 255
            elif f == 3:
                line[x] = (line[x] + ((a + b) >> 1)) & 255
            elif f == 4:
                line[x] = (line[x] + paeth(a, b, c)) & 255
            else:
                raise SystemExit("gen-oxygen-icons: %s: bad filter type %d" % (path, f))
        prev = line
        out += line[0::2] if depth == 16 else line
    return w, h, bytes(out)

def bgra(path):
    w, h, rgba = read_png_rgba(path)
    if (w, h) != (32, 32):
        raise SystemExit("gen-oxygen-icons: %s is %dx%d, expected 32x32" % (path, w, h))
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
