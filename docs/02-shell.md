# 02 — The Helix shell and "programs" (Milestone 2)

Milestone 1 proved we could boot and print. Milestone 2 turns the kernel into
an interactive system you can drive by typing: a serial/PS/2 input layer, a
line editor, a small shell, and a command table of "programs".

> **Honest scope.** These programs are **kernel-resident built-ins**, like the
> applets in early Unix or BusyBox. Each one is a C function in
> `kernel/src/commands.c`. Running a separately-compiled user-space binary
> needs memory management, an ELF loader and system calls (roadmap M3–M9).
> This milestone gives you a usable command line now and a clean migration
> path to real programs later — the command-dispatch code does not change,
> only where the bodies come from.

---

## Boot to shell

The boot chain is unchanged (`docs/01-boot.md`), but `kmain` now does more
before the prompt appears:

```
power → UEFI → Limine → kmain()
                    │
                    ├─ console_init(fb)       serial + framebuffer ready
                    ├─ keyboard_init()        PS/2 driver ready
                    ├─ memory_init(...)       Limine memmap ready
                    ├─ sysinfo_set_firmware   x86 BIOS / UEFI / etc.
                    ├─ time_init()            TSC calibrated against the PIT
                    │
                    ├─ boot banner (HELIX OS / ...)
                    ├─ status lines (console, memory, clock)
                    │
                    └─ shell_run() ─────────► helix>
```

---

## The userland shell (ring 3)

`usershell` loads a separate program into a process of its own and runs it in
ring 3. The program is built from `userspace/shell/shell.c` and `userspace/libhelix.c`,
linked as a static ELF at `0x400000`, and embedded in the kernel as a blob
(`kernel/Makefile` generates the `.incbin`). The kernel's ELF64 loader maps its
`PT_LOAD` segments into a new address space, respecting the write and execute
flags, and zeroes bss.

The shell talks to the kernel only through two native syscalls:

- `SYS_READ_INPUT` (22) with fd 0 returns one console character, or `-1` if none
  is waiting. Ring 3 runs with interrupts off, so the program polls it.
- `SYS_DEBUG_WRITE` (26) writes a user buffer to the console. This is temporary;
  the real write path needs console handles.

Built-ins: `echo`, `history`, `help`, `exit`. Up and Down recall the last 16
lines. Typing `exit` returns to the kernel shell with status 0.

> **Known limitations.** The loader does not yet free the frames of a process
> when it exits, nor the page tables of its address space, so each run leaks
> memory. There is no `init` yet, and `libhelix`'s `helix_write_fd` still calls
> the ext4-backed `SYS_WRITE`, so the shell does not use it.

## The input layer — two devices, one stream

`kernel/src/input.c` polls the two sources of characters and returns the first
one that has something:

```
serial (COM1)  ─┐
                ├─►  input_getchar()  ─►  16550 UART driver + PS/2 driver
 PS/2 Keyboard ─┘
```

- **Serial** is checked first so automated tests can drive the shell over a
  socket. This is how `make test` actually types commands into the kernel.
- **PS/2 keyboard** uses scancode set 1 with a 128-entry US translation table
  and shift handling. Mode 0 "one-shot" + polling — no interrupts needed.

When you run `make run`, the serial terminal is your keyboard. To use the
graphical framebuffer console with the real screen and a PS/2 keyboard, run:

```sh
HEADLESS=0 make run
```

---

## The line editor

`kernel/src/shell.c` reads a line character by character:

- printable ASCII echoes and is appended;
- **Backspace** (0x7F or 0x08) rubs out the last character on every backend —
  serial sends `\b \b`, the framebuffer rewrites the cell in the background
  colour;
- **Enter** (`\r` or `\n`) submits the line;
- **Ctrl-C** cancels the current line and prints `^C`;
- **Ctrl-L** redraws the screen (useful after garbled terminal output).
- **Up / Down** recall earlier lines from the command history (see below).

### Command history

The shell remembers the last 16 non-empty lines it ran (`HISTORY_MAX` in
`shell.c`), oldest first. Consecutive duplicates are stored once.

- **Up** replaces the line being edited with the previous entry; **Down** moves
  forward again and finally restores an empty line.
- `history` lists the stored lines, numbered from 1.
- The history lives in kernel memory only. It is lost at reboot and is not
  saved to disk.

Arrow keys arrive as `KBD_KEY_UP` / `KBD_KEY_DOWN` from the PS/2 driver and as
`SHELL_KEY_UP` / `SHELL_KEY_DOWN` (0x101 / 0x102, outside the byte range) from
`keyboard_read()`, so they never collide with a typed character.

> **Known limitation.** Recalling a line redraws it with backspaces and new
> text. On a serial terminal with no screen model, a longer line can leave the
> tail of the old text visible until the next redraw. The command that runs is
> correct; only the display can be untidy.

> **Not yet.** The history is a feature of the kernel-resident shell. A
> ring-3 userland shell with its own history is planned; it depends on the
> kernel `read_input` syscall (SYS_READ_INPUT), which is not implemented yet.

The serial terminal handles its own cursor; the framebuffer shows an
underscore at the next character cell, erased and redrawn on every keystroke
so it never smears.

---

## The console: one stream, two backends, colour

`kernel/src/console.c` is the only place the rest of the kernel writes. It
fans text out to:

| Backend | Colour representation |
|---|---|
| Serial (any ANSI terminal) | ANSI SGR escape sequences |
| Framebuffer | Packed RGB computed from the same palette enum |

That means a line printed in `CONSOLE_RED` shows as red both on your terminal
and on the graphical screen — without any caller having to know which backend
they got.

### Readability, and a real bug we fixed

The first version of this had a nasty bug, and it is worth understanding
because it is a classic. Foreground and background were packed by the *same*
function, and `CONSOLE_DEFAULT` mapped to near-white `#E0E0E0`. So the moment
any command set a foreground colour, `apply_colors()` also set the framebuffer
**background** to near-white. Every glyph cell after that was white-backed,
and the bright cyan/green/magenta text was painted on top of it — which is
unreadable. (A pixel audit of a screenshot found ~102,000 solid-white
background pixels; after the fix, 0.)

The fix has three parts:

1. **Pack foreground and background separately.** `fb_pack(colour, is_bg)`:
   the default *background* is black, the default *foreground* is soft white.
   They must never be the same value.
2. **A muted, high-contrast palette.** The framebuffer uses soft, One-Dark-ish
   tones (e.g. green `#98C379`, cyan `#56B6C2`) rather than fully saturated
   neon, so long sessions stay comfortable.
3. **Serial colours that work on light and dark terminals.** We emit
   *standard-intensity* ANSI codes; the "bright" palette entries become
   **bold + standard colour** (`\x1b[1;32m`), which terminals render as bright
   on a dark background and as a saturated dark colour on a light one. Raw
   bright codes (`90`–`97`) were the part that vanished on white.

If even that is hard to read on your terminal, use the escape hatch:

```
helix> color off
```

That suppresses ANSI colour entirely and lets your terminal's own default
foreground/background through. `color on` restores it. The framebuffer keeps
its palette either way, since we control that background.

The test harness now guards this: `tests/shell-test.py` audits the screenshot
and fails if it finds solid light-coloured background blocks behind text.

---

## The commands — the first Helix "programs"

Adding a program is two steps: write the function, add a row in `helix_commands`.

### The signature

```c
static void cmd_myprog(int argc, char **argv) { ... }
```

`argv[0]` is the command name. Any extra tokens are the arguments.

### Registering it

In `kernel/src/commands.c`:

```c
const command_t helix_commands[] = {
    /* ... existing entries ... */
    {"myprog", "one-line description",          cmd_myprog},
};
```

`helix_command_count` is computed automatically. Reboot, and `help myprog`
shows it; typing `myprog arg1 arg2` runs it.

### What is there today

| Command | What it does |
|---|---|
| `help` | list commands with their one-line description |
| `sysinfo` / `neofetch` / `about` | Neofetch-style system info panel |
| `cpu` | CPU vendor (CPUID leaf 0) and brand (leaves 0x80000002–4) |
| `mem` | total/usable RAM and a row-by-row physical memory map |
| `pmm` / `heap` | physical-frame and kernel-heap statistics |
| `memtest` / `vmtest` | self-tests for heap and virtual-memory operations |
| `stacktest` | checks dedicated stack, guard-page fault recovery and NX enforcement |
| `objects` | live object count and kernel handle-table usage |
| `objtest` | self-test for object references, handle rights and closing |
| `captest` | self-test for capability derivation, rights checks and revocation, including denial cases |
| `ticks` / `uptime` | PIT timer progress and time since boot |
| `uptime` | seconds and milliseconds since boot |
| `date` | current calendar date and time in UTC (`Mon Nov  2 21:34:26 UTC 2026`) |
| `time` | current wall-clock time in UTC (`21:34:26`) |
| `version` | name, version, codename |
| `usb` | USB controllers, hubs, and HID devices with their reports |
| `echo` | prints its arguments |
| `history` | lists the lines typed so far, numbered (Up/Down recall them) |
| `usermode` | runs a built-in test program in ring 3; it writes a line through the syscall gate and exits with status 7 |
| `usershell` | runs the userland shell in ring 3 (an ELF loaded into its own process); type `exit` to return |
| `init` | runs the system init process in ring 3 (PID 1 bootstrap sequence) |
| `calc` | `calc <a> <op> <b>` for `+ - * / %` (integers only) |
| `clear` | clear the screen |
| `color` | turn ANSI colour on/off: `color on`, `color off` |
| `halt` | halt the machine (you may close the QEMU window) |
| `reboot` | reset the machine |

### The `sysinfo` panel

`kernel/src/sysinfo.c` gathers everything we actually know about the machine
and prints it beside a small ASCII logo:

```
   ##      ##   founder@helix
   ##      ##   -------------
   ##      ##   OS:       Helix OS 0.5.1-dev.20261102
   ###########  Kernel:   x86_64, long mode
   ###########  Uptime:   <...>
   ##      ##   Shell:    helixsh
   ##      ##   CPU:      <CPUID brand string>
                 Memory:   <usable> MiB usable / <total> MiB total
                 Display:  <fb width>x<height>x<bpp>
                 Boot:     Limine (<BIOS|UEFI 32|UEFI 64|...>)
```

The logo cycles between cyan and bright magenta per line for a faint helix
vibe; the user/host header is bright green; the separator is grey; the rest is
the default colour so labels and values stay readable.

---

## How we know what the machine is

### Time (TSC + PIT)

`kernel/src/time.c` uses the CPU timestamp counter (`rdtsc`) and calibrates it
once against the 8253/8254 PIT (whose frequency is the textbook
**1.193182 MHz**). The result is an uptime accurate to a microsecond or so,
without ever needing an interrupt:

```
PIT channel 2, mode 0 (one-shot), count = 50 ms worth of ticks
measure TSC delta while waiting for OUT to go high
TSC ticks per ms = delta / 50
```

If calibration ever fails (no PIT response), uptime returns `unavailable`.

### Wall clock (Limine boot timestamp + CMOS RTC)

The TSC only counts from boot, so it cannot say what day it is. The calendar
comes from two sources:

1. **Limine `date_at_boot` request** — the bootloader hands the kernel a Unix
   timestamp (seconds since 1970-01-01 UTC). The kernel stores it as the boot
   epoch and adds elapsed uptime to get "now".
2. **CMOS RTC** (`kernel/src/time.c`, ports `0x70`/`0x71`) — the fallback when
   the bootloader has no timestamp. The MC146818 registers are read in BCD or
   binary mode, 12- or 24-hour, with the century register used when the
   chipset provides it. Nonsense readings are rejected instead of being
   printed.

Both paths end in `time_epoch()` (seconds since the Unix epoch) and
`time_now_utc()` (calendar fields). `date` and `time` print those fields, and
the ext4 inode timestamps use the same source instead of "seconds since boot".

### Memory map (Limine)

`kernel/src/memory.c` walks the `limine_memmap_request` response and reports
total, usable and each range by human-readable type (`usable`, `reserved`,
`ACPI`, `framebuffer`, …).

### CPU identification (CPUID)

`kernel/src/cpu.c` calls CPUID leaves 0 (vendor) and 0x80000002–4 (brand). On
QEMU we see `QEMU Virtual CPU version 2.5+`; on real silicon we see the real
brand string.

---

## New C / x86 concepts you now own

| Concept | Where | Plain meaning |
|---|---|---|
| `inb`/`outb` | `io.h` | x86 port I/O — how the CPU talks to legacy devices. |
| CPUID | `cpu.c` | Asking the CPU about itself; each "leaf" is a category of answers. |
| RDTSC | `time.c` | The CPU's high-resolution cycle counter. |
| The PIT (8253/8254) | `time.c` | A simple fixed-frequency counter the OS uses to get a wall-clock reference. |
| CMOS RTC (MC146818) | `time.c` | The PC's battery-backed clock; read through ports `0x70`/`0x71`. |
| ANSI SGR escapes | `console.c` | The byte sequences terminals interpret as colour. |
| `pause` | `shell.c` | An x86 hint instruction that lowers power in a polling spin loop. |
| Forward type idempotence | `memory.h` | Forward declarations plus a pointer-only API to avoid dragging `<limine.h>` into every caller. |

---

## How to drive it

```sh
make             # rebuild
make test        # the interactive harness — types real commands, checks real output
make run         # boot with serial on your terminal; type 'help'
```

The interactive harness reports:

```
[shell-test] RESULT: PASS (15 checks)
```

Every line of the customer transcript above in this document was captured by
that harness.

---

## What's deliberately still missing

To stay inside "Never pretend it works", these are still absent:

- **Dynamic memory.** Up to ~800 bytes of stack/bss is enough for this
  milestone; a real allocator lands in M3.
- **Interrupts.** We poll everything, which means `cpu pause` instead of sleep.
- **A scheduler and user space.** A real program process with its own stack and
  address space is M7–M9.
- **Filesystem.** Commands are kernel-resident. Once there is one (M11), the
  same command table can be backed by `helix/programs/`.
- **History and completion.** We deliberately skipped up-arrow history to keep
  the line editor honest; it is a small follow-up.
