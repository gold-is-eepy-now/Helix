# 17. SPI: the generic layer and the Intel LPSS GSPI controller

Status: **controller driver written; QEMU-tested for the absent-controller path only.**
Transfers have **not** run on any hardware. The MacBook Air 7,2 has not been tested.

## Target

- PCI **8086:9CE6**, "SPI1" at `\_SB/PCI0/SPI1` on Broadwell-U, Intel LPSS GSPI channel 1.
- Confirmed under macOS: the registry reports AppleIntelLpssGspi, the peripheral
  apple-spi-topcase, and successful transfers with no errors.
- This is the Intel serial-IO SPI block, **not** the PCH firmware-flash interface.
  No flash writes exist anywhere in this code.

## Files

- `kernel/src/spi.h`, `spi.c`: the generic layer. Controllers register with the bus,
  devices name a controller and a chip-select, and `spi_transfer()` checks arguments
  before calling the controller. Only 8-bit words are accepted.
- `kernel/src/intel_lpss_gspi.h`, `intel_lpss_gspi.c`: the Intel controller. It finds
  8086:9CE6, validates BAR0, enables memory decoding (not bus mastering), maps one page,
  resets the LPSS block, programs the SSP, reads back to verify, and only then marks the
  controller ready.
- `apple_topcase_spi.c` is **not** written. The Apple protocol is a separate stage.

## Verified register definitions and their sources

Linux kernel sources (GPL-2.0), read during this work:

| Item | Value | Source |
| --- | --- | --- |
| SSCR0, SSCR1, SSSR, SSDR offsets | 0x00, 0x04, 0x08, 0x10 | `include/linux/pxa2xx_ssp.h` |
| SSCR0_SSE | bit 7 | same |
| SSCR0_SCR | shift 8 | same |
| SSCR0_DSS | mask 0xF | same |
| SSCR1_SPO, SSCR1_SPH | bits 3, 4 | same |
| SSSR_TNF, SSSR_RNE, SSSR_BSY, SSSR_ROR | bits 2, 3, 4, 7 | same |
| LPSS private block | offset 0x200 from BAR0 | `drivers/mfd/intel-lpss.c` |
| LPSS_PRIV_RESETS | 0x04 within the private block | same |
| Reset sequence | write 0, then FUNC (0x3) \| IDMA (bit 2) | same |

## Not verified (deliberately not used or assumed)

- **Clock rate.** The SSP rate is input / (2 × (SCR + 1)), but the input clock is
  unverified. The driver programs the slowest SCR (255) and reports that. Speed
  control needs the input clock from the datasheet or from the divider in the
  private block (its formula is unconfirmed).
- **FIFO depth.** Not assumed. Each byte waits for TNF before writing and for RNE
  before reading, both bounded.
- **DSS encoding.** 8-bit is written as DSS = 7 (bits − 1). This is the usual PXA/LPSS
  encoding, but I did not confirm it against an Intel datasheet. It is the first thing
  to check if transfers misbehave on the Air.
- **Remap address and DMA-finish register.** Their offsets are not in the sources read,
  so they are not configured. Programmed I/O does not need them.
- **Chip-select framing.** The SFRM behaviour is the controller's and is unverified.
  The `chip_select` argument is accepted but not yet acted on.
- **Device protocol.** The Apple top-case device does not necessarily use standard SPI
  commands. Nothing is sent to it.

## Testing

- **Build:** `make clean && make test CC=x86_64-elf-gcc && make iso CC=x86_64-elf-gcc`.
- **QEMU (no controller):** `make test` checks that `spi` reports 8086:9CE6 as not
  found, and that boot continues.
- **MacBook Air 7,2:** boot the stick, run `pci` and `spi`, and send the output. This
  confirms the device ID and BAR0. Transfers must not be tried until `spi` reports
  `ready yes` and the readback matches.
