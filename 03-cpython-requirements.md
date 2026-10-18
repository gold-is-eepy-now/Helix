# 03 — What it takes to run CPython (and Linux/BSD software) on Helix

This is the research brief for the "can we run Python?" question. It is honest
about what exists, what does not, and what the dependency chain looks like.
Sources are listed at the end.

**Bottom line:** CPython is not the first goal — it is one of the last. It
cannot run until Helix has user space, an ELF loader, a filesystem, a C
library, and a set of POSIX/Linux system calls. The good news is that you do
not write those from scratch: you port **musl** and implement the **Linux
syscall ABI**, and then CPython, BusyBox, and most of the Linux corpus come
along for the ride.

---

## The three ways to get CPython

| Route | What it means | Cost |
|---|---|---|
| **A. Linux personality (recommended)** | Compile CPython for Linux, statically link **musl**, run the Linux ELF under a Helix syscall-translation layer. | One syscall layer + musl port. Reuses the entire Linux world. |
| **B. Native Helix port** | Make CPython's `configure` target Helix, write a Helix libc, implement `posixmodule.c` against Helix objects. | Much larger; you become the CPython platform maintainer. |
| **C. Reuse an existing POSIX OS's userland** | Not Helix; e.g. run a NetBSD userland. | Rejected — it is not our kernel's user space. |

We take **Route A**. It respects the vision ("compatibility is a runtime above
the kernel") *and* it is the only realistic path to real software quickly.

---

## Kernel prerequisites (the gate list)

CPython needs these from the kernel, in this order. Each is a milestone.

| # | Capability | Why CPython needs it | Milestone |
|---|---|---|---|
| 1 | Physical memory manager | back the heap, page tables, user memory | **M3 ✅ (done)** |
| 2 | Kernel heap (`kmalloc`) | every kernel data structure | **M3 ✅ (done)** |
| 3 | Virtual memory / page tables | per-process address spaces, `mmap` | M3 (partly) / M4 |
| 4 | Interrupts + exceptions (IDT) | page faults, timer, syscall entry | M4 |
| 5 | Threads + scheduler | `clone`, `futex`, real concurrency | M7–M8 |
| 6 | **User mode (ring 3)** | run untrusted code at all | M9 |
| 7 | **Syscall gate** (`syscall`/`sysret`) | the kernel↔user boundary | M9 |
| 8 | **ELF loader** | load the Python binary and shared libs | M9 |
| 9 | **Filesystem + VFS** | import modules, read `.pyc`, open files | M11 |
| 10 | Signals, `fork`/`exec`, `mmap`, `futex` | CPython's runtime and threads | M9–M11 |
| 11 | Networking + sockets | `socket`, `urllib`, pip | M12 |

Everything above #6 is about being able to run *any* program. #7–#11 are what
turn "a program" into "CPython".

---

## What CPython actually needs (the syscall surface)

CPython is ~500k lines of C with deep POSIX assumptions. At startup and during
normal operation it will exercise, at minimum:

| Area | Calls |
|---|---|
| Memory | `mmap`, `munmap`, `mprotect`, `brk`, `mremap` |
| Files | `openat`, `read`, `write`, `close`, `lseek`, `fstat`, `statx`, `getdents64`, `readlink`, `unlink`, `mkdir` |
| Threads | `clone` (with `CLONE_VM`), **`futex`**, `set_tid_address`, `sched_yield` |
| Processes | `execve`, `wait4`, `exit_group`, `getpid` |
| Signals | `rt_sigaction`, `rt_sigprocmask`, `sigaltstack`, `tgkill`, `rt_sigreturn` |
| Time | `clock_gettime`, `nanosleep`, `gettimeofday` |
| Random | `getrandom` |
| Misc | `uname`, `sysinfo`, `getcwd`, `ioctl` (tty), `fcntl` |

Two of these are load-bearing and hard: **`clone`** (a real scheduler with
shared address spaces) and **`futex`** (blocking wait/wake in the kernel).
Until those exist, CPython can start but cannot run threads — and modern
CPython assumes threads exist.

`getrandom` matters too: CPython seeds its hash randomisation from it, and
without it you must patch the entropy source.

---

## libc: port musl, do not write your own

Writing a libc from scratch is a multi-year detour. **musl** is small, clean,
statically linkable, and — crucially — is built *directly on the Linux syscall
layer*. Its entire OS interface is a handful of inline-assembly `__syscallN`
wrappers (one per argument count) in `arch/x86_64/syscall_arch.h`.

That means porting musl to a new kernel is mostly: **reimplement the syscall
shim, then implement the Linux syscall numbers it calls.** Everything else —
`printf`, `malloc`, string functions, `qsort`, math — is portable C that comes
along for free.

### The technique (proven by the UEFI-musl project)

musl's low-level calls go through macros `__syscallN(...)` that expand to a
`syscall` instruction. To port, you replace that instruction with a call to
your own function, passing the same registers:

```
rax = syscall number, rdi, rsi, rdx, r10, r8, r9 = arguments
```

Then you implement that number. The UEFI-musl project did exactly this for
UEFI and got working `mmap`/`munmap`/`brk`, the file calls, and `stdio`. The
same shim is how Helix will host musl: `__syscallN` → Helix's Linux
personality dispatcher → Helix native objects.

**Consequence for the design:** the "Linux personality" is a *syscall number
table*, not a fork of the kernel. One dispatcher, one entry per implemented
call, each translating Linux semantics onto Helix primitives. Unimplemented
numbers return `ENOSYS` and are logged, which turns bring-up into a checklist.

---

## The CPython porting process (from the official devguide)

The CPython developer guide is blunt: porting CPython to a new platform is an
adventure, and the guide gives only rough tips. The practical sequence it
describes:

1. Get a C compiler and a working "hello world" on the target.
2. Run CPython's `configure` (Autotools) to generate:
   - `pyconfig.h` — the platform feature definitions,
   - `config.c` — the list of built-in modules (until dynamic loading works,
     everything must be listed here),
   - a `Makefile`.
3. **Start without the `posix` module.** Comment `Modules/posixmodule.c` out of
   `config.c` and do not compile it. It is by far the biggest syscall surface.
4. Disable `site.py` (run with `-S`) and keep going until you get a `>>>`
   prompt.
5. Only then add the `os` module back, function by function.
6. Finally run CPython's own regression suite (skipping tests that need
   features you do not have — the guide points at the WASI port for how).

The important implication: **the first CPython milestone is a `>>>` prompt with
no `os` module**, and that is a perfectly respectable target.

---

## Networking prerequisites (for `socket`, `urllib`, pip)

Networking is its own milestone and does not block the `>>>` prompt, but pip
and much of the standard library need it.

1. **PCI enumeration** — find the network card (legacy I/O ports `0xCF8`/
   `0xCFC` work without paging, so this can come before VM).
2. **A NIC driver** — in QEMU, `virtio-net` (simplest to emulate) or `e1000`.
   As an isolated Helix driver service once the driver framework exists.
3. **A network stack** — ARP → IPv4 → ICMP → UDP → TCP. A lot of code, but
   very well-trodden.
4. **Sockets** — map the BSD socket calls onto Helix `NetworkInterface` objects
   behind a `Network.Connect` capability.

On the object model, a socket becomes a **channel** to a `NetworkInterface`
object, and `Network.Connect` is the capability that authorises it — which is
exactly the least-privilege story from the vision doc.

---

## The canary ladder

Do not aim at CPython directly. Aim at a ladder of existing binaries; each
rung is objective proof that the platform got more real.

| Rung | Binary | Unlocked by |
|---|---|---|
| 0 | Helix's own kernel shell | M2 ✅ (done) |
| 1 | Native Helix `hello` (static, no libc) | M9 |
| 2 | Linux static `hello` (musl, `write`+`exit` only) | minimal Linux personality |
| 3 | Linux static **BusyBox** (`sh`, `ls`, `cat`) | files, process, memory syscalls |
| 4 | musl **dynamic** `hello` | dynamic linker + VM |
| 5 | **CPython** (static musl) | `mmap`, `futex`, filesystem, time, threads |
| 6 | A BSD binary | second syscall table (shares everything else) |

Rung 3 is the real threshold: a working BusyBox shell means Helix has a
general-purpose userland. Rung 5 is CPython.

---

## Honest sequencing and effort

Milestone gates, not calendar promises:

- **M3 (now):** physical memory + heap — **done and tested.**
- **M4:** page tables, interrupts, exceptions.
- **M7–M9:** threads, scheduler, ring 3, syscalls, native ELF loader, first
  native program (ladder rung 1).
- **Then:** the Linux personality + musl port → ladder rungs 2–3.
- **Then:** filesystem, `mmap`, `futex`, signals → ladder rungs 4–5, CPython.
- **Separately:** networking → pip, sockets.

For calibration: this is a multi-year effort for a small team, and CPython sits
near the far end because it assumes threads, memory mapping, a filesystem and a
full libc all exist at once. The way to get there is one tested, documented,
booting milestone at a time — which is what we have been doing.

---

## Sources

- CPython Developer's Guide — *Porting and platform support*:
  https://devguide.python.org/developer-workflow/porting
- musl libc — *Supported Platforms* (built on the Linux syscall layer):
  https://wiki.musl-libc.org/supported-platforms
- musl — x86_64 `syscall_arch.h` (the `__syscallN` shims):
  https://git.musl-libc.org/cgit/musl/tree/arch/x86_64/syscall_arch.h
- UEFI-musl — *MUSL libc port for UEFI* (the shim-replacement technique):
  https://github.com/Openwide-Ingenierie/uefi-musl
- futex(2), clone(2) — Linux man-pages:
  https://man7.org/linux/man-pages/man2/futex.2.html,
  https://man7.org/linux/man-pages/man2/clone.2.html
- The Hobby OS Landscape (ToaruOS runs GCC + Python 3; Sortix targets POSIX):
  https://github.com/klange/hobby-os-landscape
- Redox OS — relibc, POSIX/Linux/BSD source compatibility:
  https://doc.redox-os.org/book/programs-libraries.html
