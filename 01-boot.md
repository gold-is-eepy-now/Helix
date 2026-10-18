# 01 — How Helix OS boots (Milestone 1)

This document explains, from the metal up, what happens between pressing power
and seeing the Helix banner. It is written for someone who has never built an
OS. Read it with the source open.

---

## The boot chain

```
 power on
    │
    ▼
 UEFI firmware            (on real hardware; OVMF in QEMU)
    │  looks for a bootable EFI application on the disk
    ▼
 /EFI/BOOT/BOOTX64.EFI    (the Limine bootloader)
    │  reads its config, loads an ELF kernel into memory
    ▼
 /boot/limine/limine.conf (which kernel? how to boot it?)
    │
    ▼
 /boot/kernel             (our ELF: kernel/src/*.c compiled + linked)
    │  Limine jumps to the symbol `kmain` in 64-bit long mode
    ▼
 kmain()                  (kernel/src/main.c)
    │  prints the banner, then halts forever
    ▼
 safe idle loop
```

Four different pieces of software cooperate here, and each one only knows how
to hand off to the next:

1. **UEFI firmware** knows how to read a FAT filesystem and run an EFI
   application. It knows nothing about Helix.
2. **Limine** knows how to load an ELF kernel and set up a minimal environment
   (long mode, a stack, memory information). It knows nothing about Helix's
   internals.
3. **`limine.conf`** tells Limine *which* file to load and *which* protocol to
   use.
4. **Our kernel** starts at `kmain`. It knows nothing about UEFI or Limine
   except through the small, documented interface described below.

This layering is why we use a bootloader at all: writing a UEFI application by
hand is a large amount of protocol work that teaches us nothing about operating
systems. Limine reduces it to a few lines of C.

---

## The Limine boot protocol, in one page

Limine does not just jump to us. It offers a small **protocol**: we describe
what we want, and it fills in the answers.

We declare *requests* — small structs — in a special memory section:

```c
__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0,
};
```

Then, after Limine jumps to `kmain`, we read the answer:

```c
if (framebuffer_request.response != NULL) {
    struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];
}
```

Key facts:

- Requests live in `.limine_requests` so Limine can find them by scanning the
  kernel image.
- Two **marker** arrays (`.limine_requests_start` / `.limine_requests_end`)
  bracket the list so Limine knows where it begins and ends.
- The **base revision** request tells Limine which version of the protocol we
  speak (revision 6 here). If Limine does not support it, it will not boot us;
  the check in `kmain` guards against a mismatch anyway.
- `volatile` matters: the bootloader writes into these variables *outside* the
  compiler's view. Without `volatile`, the compiler could assume the values
  never change and optimize the checks away.

`limine.h` (in `kernel/include/`) is the 0BSD-licensed protocol header that
defines all of these structs and request IDs. We vendor it rather than
reimplement it.

---

## File-by-file walkthrough

### `kernel/linker.ld` — where things go in memory

A linker script tells the linker how to lay the compiled code and data out in
the final binary. Ours does four things:

- `OUTPUT_FORMAT(elf64-x86-64)` — produce a 64-bit ELF.
- `ENTRY(kmain)` — the entry point is our C function `kmain`.
- Places the kernel at `0xffffffff80000000`, the start of the top 2 GiB. The
  Limine protocol requires the kernel in the top 2 GiB, and that lets the
  compiler use `-mcmodel=kernel` (cheaper addressing).
- Splits the image into page-aligned segments with correct permissions:
  `limine_requests` (read/write), `text` (read/execute), `rodata` (read),
  `data` (read/write).

`/DISCARD/` throws away sections we do not need and that confuse some hosts.

### `kernel/src/main.c` — the entry point

- Declares the protocol requests (base revision + framebuffer).
- `kmain()` checks the base revision, grabs a framebuffer if one exists, calls
  `console_init`, prints the banner, and enters `kernel_idle_loop`.
- `kernel_idle_loop()` executes `cli` (disable interrupts) then `hlt` (halt the
  CPU) forever. With interrupts disabled, `hlt` parks the CPU until the next
  interrupt — which never comes — so the machine sleeps instead of spinning at
  100% or executing garbage.

### `kernel/src/serial.c` — the serial console

A 16550 UART driver for COM1 (I/O port `0x3F8`). It configures the port for
38400 baud, 8N1, then writes bytes by polling the "transmit ready" bit. No
interrupts, no buffers — the simplest thing that cannot lose output. This is the
console automated tests read.

### `kernel/src/fb.c` — the framebuffer console

Renders 8×8 glyphs directly into the linear framebuffer Limine provides. It
respects the framebuffer's pitch (bytes per row), bit depth, and per-channel
bit masks, so it works on 32-bit XRGB/BGRX and 24-bit RGB alike. It tracks a
text cursor, wraps at the right edge, and scrolls by copying the image up one
line when the cursor reaches the bottom.

### `kernel/src/console.c` — one output channel

Fans text out to every available backend: serial always, framebuffer if
present. The rest of the kernel calls `console_write` and never needs to know
where the bytes go. Also provides `console_write_dec` and `console_write_hex`
for diagnostics.

### `kernel/src/font8x8.h` — the font

A public-domain 8×8 bitmap font (128 glyphs, basic Latin). Each glyph is eight
bytes; bit *n* of a byte is the pixel in column *n*.

### `tools/mkimage.sh` — building the disk

Creates a 64 MiB disk with a GPT and one EFI System Partition (FAT), then copies
in `BOOTX64.EFI`, `limine.conf`, and the kernel. The firmware finds
`/EFI/BOOT/BOOTX64.EFI` automatically — the UEFI "removable media" boot path.

### `tests/boot-test.sh` — proving it boots

Boots the image headlessly under QEMU + OVMF, captures serial output, requires
the three banner lines, and takes a framebuffer screenshot. This is the
machine-checked definition of "Milestone 1 works".

---

## New C concepts used here

| Concept | Where | What it means |
|---|---|---|
| `volatile` | `main.c` requests | The value can change without the compiler's knowledge; do not cache or optimize it. |
| `__attribute__((used))` | `main.c` requests | Keep this symbol even if nothing appears to use it. |
| `__attribute__((section(".x")))` | `main.c` requests | Put this variable in a named section, not the default one. |
| Linker sections | `linker.ld` | Named regions of the final binary; the linker script decides their addresses and permissions. |
| Inline assembly | `serial.c`, `main.c` | Embed raw `in`/`out`/`hlt` instructions the C language cannot express. |
| Freestanding C | `kernel/Makefile` | C with no hosted library: no `printf`, no `malloc`, no assumptions about the runtime. |
| Pointer-to-struct access | `main.c` | `response->framebuffers[0]` reads through pointers the bootloader filled in. |
| Fixed-width types | everywhere | `uint32_t` etc. make the exact size explicit — critical when talking to hardware. |

---

## How to test it

```sh
make test
```

Expected (real captured output):

```
HELIX OS
Native Object Kernel
Boot successful.

[helix] console: serial online + framebuffer 1280x800x32
[helix] arch: x86_64, long mode
[helix] state: entering safe idle loop
```

`make test` also writes `build/test/screen.png`, a screenshot of the graphical
console.

To watch it interactively:

```sh
make run        # serial output on your terminal
```

---

## Deliberate limitations

Milestone 1 does **not** implement: memory management, a scheduler, IPC, an
object manager, capabilities, drivers, filesystems, networking, USB, a GUI, or
package management. There is no way to run a program yet. Those are milestones
M2 onward (see [ROADMAP.md](../ROADMAP.md)). The point of M1 is to prove the
boot chain end to end, and it does.
