# 19. xHCI (USB 3) host controller: stage 1, read-only discovery

Status: **discovery only; QEMU-tested with `qemu-xhci`.** Not run on the MacBook Air 7,2.
No keyboard or other device works through xHCI yet.

## What stage 1 does

- Finds the controller by PCI class `0C/03/30`.
- Maps BAR0 (64-bit aware) into a fixed 64 KiB virtual window.
- Decodes the capability registers: version, slots, interrupters, ports.
- Reads each port's `PORTSC`: connected (bit 0) and powered (bit 9).
- Writes **nothing** to the controller. No reset, no ring, no command.

The `xhci` shell command prints the result. Boot logs one `[xhci]` line.

## Sources for the offsets and fields

From the Linux xHCI headers (`drivers/usb/host/xhci-caps.h`, `xhci.h`) and the OSDev xHCI page:

| Item | Value |
| --- | --- |
| CAPLENGTH | 0x00, byte; operational registers start at base + CAPLENGTH |
| HCIVERSION | 0x02, 16 bits |
| HCSPARAMS1 | 0x04. MaxSlots bits 0-7, MaxIntrs bits 8-18, MaxPorts bits 24-31 |
| HCSPARAMS2 | 0x08 |
| HCCPARAMS1 | 0x10 |
| DBOFF | 0x14, mask ~3 |
| RTSOFF | 0x18, mask ~0x1F |
| USBSTS | operational + 0x04 |
| PAGESIZE | operational + 0x08 (raw value logged, not interpreted) |
| Port registers | operational + 0x400, stride 0x10 |
| PORT_CONNECT / PORT_POWER | bit 0 / bit 9 of PORTSC |

## Not verified, and not used yet

- **Run/Stop and HCRST (reset) bits.** HCRST is bit 1 of USBCMD in the Linux header. Run/Stop
  and the controller-not-ready bit (CNR) were not in the sources I could fetch, so no reset is
  done. Stage 2 needs them confirmed first.
- **TRB layout and the cycle bit.** Needed for the command and event rings, which are stage 2.
- **Interrupts, slots, and device contexts.** Later stages.

## Known issue

- Under `qemu-xhci`, the HCIVERSION field reads as `0x0000`. QEMU normally reports `0x0100`.
  The port and slot counts read correctly, so the register window is right. The cause is not
  explained yet; nothing depends on the version field.

## Testing

- **QEMU with a controller:** `qemu-system-x86_64 ... -device qemu-xhci -device usb-kbd`.
  The boot log should show `[xhci] found at ...` and the connected port count.
- **QEMU without one:** `make test` checks that `xhci` prints a status line.
- **MacBook Air 7,2:** after boot, the same read-only read shows what the Air's xHCI reports.
  It's the safest place to start on real hardware, because it writes nothing.
