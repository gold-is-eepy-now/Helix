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


def dword(v):
    return b"\x0c" + struct.pack("<I", v)                             # DWordPrefix


def buffer(data):
    content = byte(len(data)) + data                                  # BufferSize, then the bytes
    return b"\x11" + pkg_length(len(content)) + content               # BufferOp


# A _CRS with one SpiSerialBus descriptor (ACPI 6.x, 6.4.3.8.2) and the end tag. Chip select 0,
# clock phase 0 and polarity 0, 8-bit words, 1,000,000 Hz, 4-wire. The layout is the one in
# kernel/src/acpi_spi_res.h; the parser's host test checks the same bytes.
SPI_CRS = bytes([
    0x8E, 0x12, 0x00,                 # large item 0x0E (serial bus), length 18
    0x01, 0x00, 0x02, 0x00,           # revision 1, source index 0, bus type 2 (SPI), flags 0
    0x00, 0x00,                       # type-specific flags: 4-wire, active-low chip select
    0x01, 0x09, 0x00,                 # type revision 1, type data length 9
    0x40, 0x42, 0x0F, 0x00,           # connection speed 1000000
    0x08, 0x00, 0x00,                 # 8 bits, clock phase 0, clock polarity 0
    0x00, 0x00,                       # device selection 0
    0x79, 0x00,                       # end tag
])

ARG0 = b"\x68"
ZERO = b"\x00"

# SPI1 is the LPSS controller in the ACPI tree (_ADR 0x001E0002: device 0x1E, function 2). APPT
# hangs under it, so the link from the device to its controller is the one the real machine has.
aml_body = scope(b"\x5c" + seg("_SB"), device("SPI1",
    name("_ADR", dword(0x001E0002)) +
    device("APPT",
        name("_HID", string("APP000D")) +
        name("_GPE", byte(0x10)) +
        name("_CRS", buffer(SPI_CRS)) +
        method("UIST", 0, ret(ZERO)) +
        method("SIST", 0, ret(byte(1))) +
        method("SIEN", 1, ret(ARG0)))))

header_len = 36
length = header_len + len(aml_body)
header = (b"SSDT" + struct.pack("<I", length) + bytes([2]) + b"\x00" +   # revision 2, checksum later
          b"HELIX " + b"APPLETST" + struct.pack("<I", 1) + b"HLX " + struct.pack("<I", 1))
table = bytearray(header + aml_body)
table[9] = (-sum(table)) & 0xFF                                           # all bytes sum to 0

with open(sys.argv[1], "wb") as f:
    f.write(bytes(table))
print(f"gen-applespi-ssdt: wrote {sys.argv[1]} ({len(table)} bytes)")
