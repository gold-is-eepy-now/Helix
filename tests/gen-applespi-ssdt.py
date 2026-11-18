#!/usr/bin/env python3
"""Writes a synthetic SSDT for the test image: a device \\_SB.APPT with _HID "APP000D", the
id the Apple SPI keyboard uses, and the ACPI methods applespi calls (UIST, SIST, SIEN). It is
loaded with QEMU's -acpitable only in tests (tests/shell-test.py); it never ships in the image.

The methods are the smallest that let the driver get past its ACPI calls: UIST says USB is off,
SIST says SPI is already on (so no delay), and SIEN accepts its argument. Usage:
    gen-applespi-ssdt.py OUTPUT.aml
"""
import struct
import sys


def pkg_length(n):
    """ACPI PkgLength for content of n bytes. The encoded length includes its own bytes."""
    if n + 1 <= 63:
        return bytes([n + 1])
    total = n + 2
    assert total <= 0xFFF, "content too long for the 2-byte encoding"
    return bytes([0x40 | (total & 0x0F), (total >> 4) & 0xFF])


def seg(name):
    return name.ljust(4, "_").encode()


def device(name, body):
    content = seg(name) + body
    return b"\x5b\x82" + pkg_length(len(content)) + content          # DeviceOp


def scope(target, body):
    content = target + body
    return b"\x10" + pkg_length(len(content)) + content              # ScopeOp


def name(seg_name, data):
    return b"\x08" + seg(seg_name) + data                             # NameOp


def string(s):
    return b"\x0d" + s.encode() + b"\x00"                             # StringPrefix


def byte(v):
    return b"\x0a" + bytes([v])                                       # BytePrefix


def method(seg_name, argc, body):
    content = seg(seg_name) + bytes([argc & 0x07]) + body             # flags: arg count only
    return b"\x14" + pkg_length(len(content)) + content               # MethodOp


def ret(data):
    return b"\xa4" + data                                             # ReturnOp


ARG0 = b"\x68"
ZERO = b"\x00"

aml_body = scope(b"\x5c" + seg("_SB"), device("APPT",
    name("_HID", string("APP000D")) +
    name("_GPE", byte(0x10)) +
    method("UIST", 0, ret(ZERO)) +
    method("SIST", 0, ret(byte(1))) +
    method("SIEN", 1, ret(ARG0))))

header_len = 36
length = header_len + len(aml_body)
header = (b"SSDT" + struct.pack("<I", length) + bytes([2]) + b"\x00" +   # revision 2, checksum later
          b"HELIX " + b"APPLETST" + struct.pack("<I", 1) + b"HLX " + struct.pack("<I", 1))
table = bytearray(header + aml_body)
table[9] = (-sum(table)) & 0xFF                                           # all bytes sum to 0

with open(sys.argv[1], "wb") as f:
    f.write(bytes(table))
print(f"gen-applespi-ssdt: wrote {sys.argv[1]} ({len(table)} bytes)")
