# 04 — Paging, interrupts and the timer (Milestone 4)

Milestone 3 gave the kernel physical memory. Milestone 4 gives it **virtual
memory it controls**, a **CPU exception handler that explains what went wrong**,
and a **live timer interrupt**. It also produced two genuinely instructive bugs,
both documented below.

### Physical frame allocator storage

The PMM sizes its bitmap from the highest usable physical frame in Limine's
memory map. It stores the bitmap in a contiguous usable run reachable through
the HHDM, marks those backing frames reserved, and includes them in the
allocated-frame count. `pmm_bitmap_bytes()` reports the actual bitmap size;
managed memory is no longer capped by a fixed 4 GiB array. Initialization
fails closed if the bitmap cannot be placed or the memory map/HHDM is missing.

> **Honest scope.** We now own page tables and take hardware interrupts. We do
> **not** yet have a scheduler, threads, user space, or `mmap` for programs.
> Those build on exactly this foundation.

---

## 1. Virtual memory: our own page tables

x86_64 translates every virtual address through four levels of tables:

```
virtual address = [ PML4 idx | PDPT idx | PD idx | PT idx | 12-bit offset ]
                       9 bits      9 bits    9 bits   9 bits
```

Each table has 512 8-byte entries. An entry either points to the next table
down, or — at the last level — to a physical page, plus flag bits (present,
writable, user, no-execute, …).

Limine already built a table for us. Its physical address is in **CR3**. We
adopt that table and modify it in place (`kernel/src/vmm.c`):

- `vmm_map(virt, phys, flags)` — walk the four levels, allocating and zeroing a
  frame for any missing table, then write the leaf entry.
- `vmm_unmap(virt)` — clear the leaf entry.
- `vmm_protect(virt, flags)` — rewrite the leaf entry's flags.
- `vmm_get_mapping(virt, &phys, &flags)` — read a mapping back (used by tests).

Two details matter:

- **We reach page tables through the higher-half direct map.** A table's
  address in the tables is *physical*; to read or write it we add the Limine
  HHDM offset. `table_at(phys) == (uint64_t *)(phys + hhdm)`.
- **Every change must flush the TLB.** After editing an entry we execute
  `invlpg (virt)`, or the CPU may keep using the stale translation.

We also handle **2 MiB huge pages**: if a page-directory entry is a huge page
and we need to map inside it, `page_table()` splits it into 512 normal pages
first. (1 GiB huge pages at the PDPT level are not handled yet.)

### The self-test

`vmtest` allocates a frame, maps it at a scratch virtual address, writes and
reads a pattern, checks the mapping, flips it read-only, unmaps it, and frees
the frame. Real captured output:

```
helix> vmtest
Virtual memory self-test
  frame 0x0000000000142000 -> virtual 0xffff900000000000
  map: ok
  write/read: ok
  query: ok (phys 0x0000000000142000, flags 63)
  protect (read-only): ok
  unmap: ok
  result: PASS
```

`flags 63` is `0x3F`: present (0x1) + write (0x2) + user (0x4) + accessed (0x20)
+ dirty (0x40) — the CPU sets accessed/dirty bits itself when the page is used.

---

## 2. The IDT and readable exceptions

The **Interrupt Descriptor Table** maps the 256 interrupt vectors to handlers.
Each 16-byte gate holds the handler address, a code-segment selector, and
attributes. `idt.c` points every vector at a default stub, then installs the
32 CPU-exception stubs and 16 IRQ stubs.

The stubs live in `kernel/src/isr.S`. The CPU pushes an interrupt frame
(`RIP`, `CS`, `RFLAGS`, and an error code for some exceptions); our stubs push
a dummy error code when there is none, then the vector number, then fall into
`isr_common`, which saves all general-purpose registers and calls
`isr_handler(registers_t *)`. The frame layout is a plain C struct in `idt.h`,
so the handler can read it directly.

Exceptions print a report and halt. Captured from a deliberate null
dereference:

```
helix> fault
Triggering a deliberate page fault (null dereference)...

*** CPU EXCEPTION 14: Page fault ***
  error code : 0x0000000000000002
  RIP        : 0xffffffff80001262   CS: 0x0000000000000028   RFLAGS: 0x0000000000000286
  fault addr : 0x0000000000000000 (from CR2)

*** KERNEL PANIC ***
unhandled CPU exception
System halted.
```

That is the difference between "it doesn't work" and "it says exactly what
went wrong and where." The error code's bit 1 tells us it was a *write* fault.

---

## 3. The PIC and the PIT timer

**IRQ remapping.** By default the 8259 PIC delivers IRQ0–7 as vectors 8–15,
which collide with CPU exceptions. `pic.c` sends the standard ICW sequence to
remap them to vectors 32–47, and exposes mask/unmask and EOI helpers.

**The timer.** `pit.c` programs PIT channel 0 in mode 3 at 100 Hz. Each IRQ0
lands in `isr_handler`, which calls `pit_tick()` and acknowledges the PIC.
Captured:

```
helix> ticks
PIT timer: 100 Hz, 632 ticks, 6320 ms
```

---

## 4. Two bugs worth remembering

### 4.1 The code-segment selector is not always `0x08`

Every interrupt was silently ignored. The IDT was loaded correctly (verified
with `sidt`), the stubs were correct (verified with `objdump`), and the PIT was
counting — but nothing fired. Reading the actual registers revealed:

```
CS = 0x28   SS = 0x30
```

**Limine's GDT does not use the `0x08`/`0x10` layout that most tutorials
assume.** Our gates said `selector = 0x08`, which is not a valid 64-bit code
segment in this GDT, so the CPU rejected every interrupt.

The fix is not to hardcode `0x28` (another bootloader may differ) but to use
the selector the CPU is *already running with*:

```c
#define KERNEL_CODE_SELECTOR read_cs()
```

### 4.2 The direct map covers RAM, not MMIO

With the selector fixed, exceptions worked but the hardware timer still never
fired. The PIT counter was advancing and a software `int $32` incremented the
tick counter, so the only missing link was delivery from the PIC to the CPU:
the local APIC was enabled, and its LINT0 pin (which carries the 8259's
output) was not routing. We wrote a small `apic_allow_pic()` to set LINT0 to
ExtINT mode and drop the task priority to 0.

The first attempt immediately page-faulted — and the new exception handler
told us exactly why:

```
*** CPU EXCEPTION 14: Page fault ***
  fault addr : 0xffff8000fee000f0 (from CR2)
```

`0xffff8000fee00000` is the HHDM address of the LAPIC (`0xfee00000`). **The
higher-half direct map covers physical RAM, but not device MMIO.** Device
registers must be mapped explicitly:

```c
vmm_map(apic_virt, apic_phys, VMM_PRESENT | VMM_WRITE | VMM_NX);
```

This is a lesson that recurs for every driver: MMIO is not RAM, and the
kernel's map does not include it for free.

### 4.3 A build-system bug that hid for three milestones

While adding `.S` support to the kernel Makefile, we found that an earlier edit
had **joined two lines**:

```make
SOURCES := $(wildcard $(SRC_DIR)/*.c)OBJECTS := $(patsubst ...)
```

`OBJECTS` was therefore empty, and `make kernel` quietly did nothing (the old
binary was "up to date"). The fix splits the lines, adds `.S` support, and adds
a guard so an empty source list is a hard error:

```make
ifeq ($(strip $(OBJECTS)),)
$(error No kernel sources found in $(SRC_DIR))
endif
```

---

## 5. New shell commands

| Command | What it does |
|---|---|
| `vmtest` | map / write / query / protect / unmap a page; prints PASS/FAIL |
| `ticks` | PIT frequency, tick count and derived milliseconds |
| `swint` | software-trigger vector 32 (diagnostic) |
| `pitcount` | read the raw PIT counter (diagnostic) |
| `fault` | deliberately trigger a page fault to exercise the handler |

---

## 6. New C / x86 concepts you now own

| Concept | Where | Plain meaning |
|---|---|---|
| Four-level paging | `vmm.c` | How x86_64 turns a virtual address into a physical one. |
| CR2 / CR3 | `io.h` | CR3 = the page table root; CR2 = the address that faulted. |
| `invlpg` | `io.h` | Flush one stale TLB translation after editing a page table. |
| Interrupt frame | `isr.S`, `idt.h` | What the CPU pushes when an interrupt occurs. |
| `iretq` | `isr.S` | The instruction that returns from an interrupt. |
| GDT code selector | `idt.c` | Which code segment the CPU must use to run the handler. |
| PIC ICW sequence | `pic.c` | How to move hardware IRQs out of the exception vector range. |
| PIT divisor | `pit.c` | `1193182 / hz` gives a periodic tick. |
| `rdmsr`/`wrmsr` | `io.h` | Reading/writing CPU model-specific registers (e.g. APIC base). |
| MMIO vs RAM | `apic.c` | Device registers are memory, but not part of the RAM map. |

---

## 7. How to test it

```sh
make test        # 45 checks: boot, shell, memory, paging, stack, objects, capabilities
```

`make test` now also verifies that the tick count is genuinely advancing (not
just that the command exists) and that a deliberate fault is caught and
reported. Everything else from M2 and M3 still passes.

---

## 8. What's next

`stacktest` checks the dedicated 32 KiB stack, deliberately reads the
non-present guard page, and tries to execute a page mapped NX. The page-fault
handler recovers from both probes and the command checks their x86 fault codes.
The test suite now covers these M3 checks.

The M3 exit gate is complete, and the M5 object manager exit gate passes. Work
continues with **M6 — capabilities**, built on those object and handle APIs.
