# 18. Apple top-case device: read-only probe

Status: **probe written; QEMU-tested for the refusal path only.** Not run on the MacBook Air 7,2.

## What the probe does

`topcase` (shell command) clocks one 256-byte frame out of the device on SPI channel 1
with the output held at zero, and prints:
- how many bytes were 0x00 and 0xFF,
- the first 16 bytes,
- the last two bytes (the stored CRC) against the CRC-16 we compute over the first 254.

It sends no commands. It refuses to run unless the GSPI controller reports ready (see docs/17-spi.md).

## Settings (from the macOS registry, `SPIT@0`, device `APP000D`)

- Mode 0: `spiSPO` 0, `spiSPH` 0.
- 8-bit words: `spiWordSize` 8.
- `spiBitOrder` 1: meaning not confirmed.
- `spiSclkPeriod` 125: unit not confirmed. The controller runs at its slowest rate regardless.
- `spiCSDelay`, `resetA2RUsec`, `resetRecUsec`: 10 each, unit likely µs. Not used yet.

## Not verified

- **CRC.** CRC-16/ARC (init 0, reflected 0xA001) is taken from the Linux driver's use of
  `crc16(0, ...)`. A MATCH on a real frame would confirm it; a mismatch means either the
  polynomial or the frame layout is wrong.
- **Chip-select framing.** The controller's own SFRM is assumed to assert during a
  transfer. If the device stays silent, this is the first thing to check.
- **Keyboard enable.** The `SIEN`/`SIST` ACPI methods, which macOS and Linux call, are
  not in our sources, so the probe does not enable the device.

## Testing

- **QEMU:** `make test` checks that `topcase` refuses to run without the controller.
- **MacBook Air 7,2:** after `spi` reports `ready yes`, run `topcase` and send the output.
