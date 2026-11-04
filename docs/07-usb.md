# 07 — USB: host controllers, hubs and HID input

This is the USB stack as it stands in Helix 0.6 ("Handshake"). It is written so
you can follow it from the hardware up: why the chipset looks the way it does,
what the controller does with our memory, and how the keyboard reaches the
shell.

## Status, in one table

| Part | State |
|---|---|
| PCI configuration access (`pci.c`) | ✅ QEMU-tested |
| UHCI host controller: reset, frame list, schedule | ✅ QEMU-tested |
| Root ports: reset, enable, enumerate | ✅ QEMU-tested |
| Hubs on a root port, devices behind them (nested hubs supported by the code; QEMU-tested one level only) | ✅ QEMU-tested |
| EHCI handoff: EHCI reset so its ports belong to the companions | ✅ QEMU-tested |
| HID boot keyboard: interrupt endpoint, report decoding, key events | ✅ QEMU-tested (plus host unit tests) |
| HID boot mouse: report decoding | ✅ host unit tests; enumerated in QEMU; no consumer yet |
| Apple iMac 11,2 hardware | ⛔ **not tested** (no hardware here) |
| Hot-plug, hub status-change interrupts | ⛔ not implemented; devices are found at boot only |
| USB storage | ⛔ not implemented on this stack |

The tests that back the ✅ rows are described at the end of this document.

## Why this is UHCI, not xHCI

USB 2.0 has two kinds of host controller in a PC chipset:

- **EHCI** runs high-speed (480 Mb/s) devices. A high-speed device on an EHCI
  port is handled by EHCI.
- **UHCI** (and OHCI) run full-speed (12 Mb/s) and low-speed (1.5 Mb/s) devices.
  Every EHCI controller has these as *companions*. An EHCI port that sees a
  full-speed device hands it to a companion: after the port reset the EHCI
  controller clears the port's enable bit and sets its "owner" bit, and the
  companion takes over.

A keyboard, a mouse, and a hub are almost always full- or low-speed, so on an
Intel PCH they are visible only through UHCI. Intel's 2009/2010 chipsets
(the **Ibex Peak** PCH in the iMac 11,2) have two EHCI controllers and **six**
UHCI companions, and no xHCI at all. So the iMac needs a UHCI driver.

We do not try to drive EHCI for devices, because most of the things that
matter on that machine are not high-speed. Instead `usb_host.c` resets each
EHCI controller. A reset controller gives every port to its companions, and
UHCI then drives all of them. The cost is that a high-speed device is
enumerated at full speed. That is slower, but a keyboard and mouse do not
notice, and it removes the need for split transactions (the transaction
translator) in the first version.

## The chipset layout we test against

```
              EHCI 00:1d.7 ── ports 1..6 ── UHCI 00:1d.0, 1d.1, 1d.2 (companions)
  root ──┤
  complex     EHCI 00:1a.7 ── ports 1..6 ── UHCI 00:1a.0, 1a.1, 1a.2 (companions)
```

Helix's driver sees the same thing a real Intel PCH shows: each UHCI controller
has two root ports. The iMac's rear USB ports and the keyboard's internal hub
sit behind these.

The QEMU test (`tests/usb-test.py`) builds exactly this topology with
`ich9-usb-ehci1/2` and `ich9-usb-uhci1..6`.

## What runs at boot, in order

1. **`pci_find_class`** (`pci.c`) scans PCI configuration space for class
   `0C03`, programming interface `20` (EHCI) and then `00` (UHCI).
2. **`ehci_release`** (`usb_host.c`) maps each EHCI controller's register page
   and gives its ports to the companions:
   - it takes the BIOS/SMM handoff (the "USB legacy support" capability), so the
     firmware stops owning the controller;
   - it stops the controller (clears `USBCMD.RS`) and waits for `HCHalted`;
   - it resets the controller (`USBCMD.HCRESET`). A reset controller has
     `CONFIGFLAG` cleared, so every port belongs to the companions.
3. **`uhci_start`** for each UHCI controller:
   - reads its I/O base from BAR4 and enables I/O decoding and bus mastering;
   - allocates three physical pages: the 1024-entry frame list, the queue
     heads and transfer-descriptor pool, and a control-transfer buffer;
   - resets the controller (global reset, then host-controller reset);
   - builds the schedule: `frame[i] → qh_int → qh_ctl → end`. The interrupt
     queue head sits in front of the control queue, so interrupt polling and
     control transfers share the frame list without interfering;
   - sets `USBCMD.RS` to run it.
4. **`root_port`** for each of the two root ports: if a device is connected,
   reset the port, enable it, read its speed (`PORTSC.LSDA`), and enumerate it.
5. **`enumerate`** gives one device an address. A new device always answers at
   address 0, so the sequence is:
   - read 8 bytes of the device descriptor at address 0 to learn `bMaxPacketSize0`;
   - `SET_ADDRESS` to a free address (1..127);
   - read the full device descriptor, then the configuration descriptor;
   - walk the configuration: a hub class or a boot HID interface, and its
     interrupt IN endpoint;
   - `SET_CONFIGURATION`;
   - for a HID boot device, `SET_PROTOCOL(boot)` and `SET_IDLE(0)`, then arm the
     interrupt endpoint;
   - for a hub, `hub_start`.
6. **`hub_start`** reads the hub descriptor, powers every port, waits for power
   to settle, then for each connected port: clears the connect-change bit,
   resets the port, waits for the reset-complete bit, clears it, and enumerates
   whatever is behind it. Nothing needs a route string: a hub forwards traffic
   for address 0 to the one port that is reset and enabled, and we enumerate one
   port at a time.

`usb_host_init` is called from `kernel_main_continue`, after `vmm_init`. The EHCI
registers must be mapped by our own page tables before we can reach them (see
*Two bugs we hit* below), and this way USB starts before the shell takes the
keyboard.

## How a UHCI transfer works

UHCI does not have a register per transfer. Software builds **transfer
descriptors** (TDs), links them into **queue heads** (QHs), and the controller
walks the frame list once a millisecond:

- A TD says *what to do*: a PID (`SETUP`, `IN`, `OUT`), the device address and
  endpoint, a data toggle, a maximum length, and a buffer address. Its status
  word holds the result. The `Active` bit means "not done yet"; the controller
  clears it when the transaction completes or fails, and writes the length
  actually transferred.
- A QH points at the TD it is currently working on (its *element* link) and at
  the next QH (its *horizontal* link).

A control transfer is three stages in the TD chain: a `SETUP` packet of 8 bytes,
one data TD per packet (UHCI has no packet-size field, so the driver splits the
data stage at `bMaxPacketSize0`), then a zero-length status TD in the opposite
direction. The data toggle starts at `DATA1` for the data stage and alternates
per packet; the status stage is always `DATA1`.

An interrupt endpoint is polled by the controller, not by us. One TD sits on the
interrupt QH with `Active` set. The device answers `NAK` each frame until it has
a report, and the controller completes the TD when one arrives. We poll the
status word (`usb_host_poll`), copy the report out, flip the toggle, and re-arm
the TD.

## HID: from report bytes to key events

A HID **boot** keyboard sends an 8-byte report on every change:

```
byte 0: modifiers   bit0 LCtrl bit1 LShift bit2 LAlt bit3 LGUI  bit4 RCtrl bit5 RShift bit6 RAlt bit7 RGUI
byte 1: reserved
byte 2-7: up to six key usages currently held (0 = empty, 0x01 = rollover error)
```

`usb_hid.c` keeps the previous report and diffs it: a usage that is new is a
press, a usage that has gone is a release. On press we record which key the
usage produced (so the release matches even if Shift was let go in between),
and we queue a `keyboard_event_t`. The event type is the one the PS/2 driver
already uses, so the shell cannot tell them apart.

Boot keyboards do not auto-repeat. `usb_hid.c` repeats a held key itself, after
500 ms, every 33 ms, the same timing a PC BIOS uses. Caps Lock toggles on the
press edge of its usage.

A boot mouse sends `buttons, dx, dy` (dx and dy are signed bytes). We accumulate
motion between reads. Nothing consumes it yet; that is a design decision (see
*Not yet*).

`keyboard_read_event` (in `keyboard.c`) now calls `usb_host_poll()` first, then
takes a USB event if there is one, and only then reads the PS/2 port. On a
machine with no USB, `usb_host_poll()` finds no devices and returns at once.

## Two bugs we hit while building this

These are the kind of thing that does not show up in a plan, so they are worth
recording.

**1. The EHCI registers were not mapped.** The first build booted QEMU with an
EHCI controller and the kernel printed nothing at all. The fault was in
`ehci_release`: it read the EHCI capability registers through Limine's
higher-half direct map (HHDM), and the firmware had placed the EHCI BAR in the
PCI memory hole, which the HHDM does not cover. The fix is to map the page
ourselves with `vmm_map`, as uncacheable and non-executable, and to start USB
after `vmm_init`. The symptom was misleading because the kernel's banner is
printed after USB starts, so a crash there looks like a dead boot.

**2. Typing into QEMU reached the PS/2 keyboard, not the USB one.** QEMU's
`q35` machine always has an i8042 PS/2 controller, and `-nodefaults` does not
remove it. So a test that types with `sendkey` passed even with no USB keyboard
attached. The test now checks the kernel's own count of USB reports (the `usb`
command), and a negative-control scenario with no keyboard must not report one.

## The `usb` command

```
helix> usb
usb: 2 uhci controller(s), 3 device(s)
  address 1: hub, full-speed
    (lists nothing: hubs have no interrupt endpoints we poll)
  address 2: device, full-speed
    interface 0 HID keyboard: reports 412, errors 0, status 0x..., last 00 00 04 00 00 00 00 00
    interface 1 HID mouse: reports 0, errors 0, status 0x..., last 00 00 00 00 00 00 00 00
```

(The layout is real; the numbers here are illustrative.) Every HID boot interface
on a device gets its own endpoint, so a composite device whose keyboard is not
its first interface still has its keyboard armed. `reports` counts reports
delivered to the HID decoder, `errors` counts failed interrupt transfers, and
`last` shows the first eight bytes of the most recent report. If a keyboard
shows `reports 0` after you press keys, the interrupt transfers are not
completing. If it shows reports with a `last` that looks wrong, the decoder is
receiving something other than boot-protocol reports.

## Testing

Run by `make test`:

- **`tests/shell-test.py`** — the shell, as before.
- **`tests/usb-test.py`** — boots Helix with the Ibex Peak topology (two EHCI,
  six UHCI, a hub) and four scenarios:
  1. a keyboard behind a hub (the iMac 11,2 layout): the log shows the hub and
     the keyboard, and typing produces USB reports;
  2. a keyboard directly on a root port: the same check without a hub;
  3. a mouse behind a hub: enumerated as a HID mouse;
  4. a negative control with a hub and no keyboard: no keyboard is reported.
  QEMU's `sendkey` also reaches the built-in PS/2 keyboard, so shell output alone
  cannot prove USB input. The report count from the `usb` command is the
  evidence (see above).
- **`tests/usb-hid-unit.c`** — the HID decoder on the build machine: plain and
  shifted letters and digits, Caps Lock, Enter, Backspace, arrows, modifier
  events, rollover, auto-repeat timing, and mouse motion accumulation.
- **`make test`** also runs the runtime tests (unchanged).

## Not yet

- **Real hardware.** Nothing here has run on an iMac 11,2. The QEMU topology
  matches its chipset, but Apple's firmware, its USB quirks, and the keyboard's
  internal hub have not been tried.
- **Hot-plug and hub status changes.** Devices are found at boot. We do not poll
  the hub's status-change interrupt endpoint.
- **Full-speed only.** Because EHCI is reset, high-speed devices run at full
  speed, and there is no transaction translator support.
- **Nested hubs** are written but only one hub level is tested in QEMU.
- **Mouse consumer.** Motion and buttons are decoded but no cursor or scroll
  handler uses them. Deciding what the pointer should do is a design choice for
  the owner.
- **Function keys** (F1–F12) and keypad keys are not mapped.
- **Storage** is not on this stack. The earlier xHCI/EHCI storage code from the
  other tree has not been merged into this one.
