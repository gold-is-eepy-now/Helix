# runtime/
User-space runtimes for Helix.

The **Linux/BSD runtime** (this directory) hosts Unix ELF binaries in
userspace and translates their system calls onto Helix's native syscall
ABI (`int $0x80`). The **POSIX layer** remains planned.

These runtimes sit **above** the kernel. They translate Unix behaviour into
Helix's native model; they do not change the kernel's native design.

## What is here

- **ELF loader** (`elf_loader.c`) — parses ELF64 headers and PT_LOAD program
  headers for x86_64 ET_EXEC/ET_DYN binaries (Linux and BSD share this
  format; only `e_ident[7]` OSABI differs).
- **Linux syscall translation** (`linux_syscall.c`) — direct-indexed table
  covering every known x86_64 Linux syscall number (0..511). Translates via
  `native_syscall_invoke()` → `int $0x80` with the kernel's register ABI
  (rax = native number, rdi/rsi/rdx/r10/r8/r9 = args).
- **BSD syscall translation** (`bsd_syscall.c`) — FreeBSD/NetBSD/OpenBSD
  numbering (3=read, 4=write, 5=open, 6=close, 1=exit, 189=brk) onto the
  same native syscalls, sharing the Linux FD table.
- **File descriptor layer** (`linux_runtime.c`) — maps Linux/BSD fds to
  Helix handles (`linux_fd_to_handle` / `linux_set_fd_handle`).
- **Entry point** (`linux_runtime_main.c`) — runtime process startup and
  ABI dispatch stub.

## Syscall translation table

All known Linux x86_64 syscall numbers are covered: those with a native
Helix equivalent are mapped, everything else returns ENOSYS (-38). Invalid
Linux FDs return EBADF (-9) before any native call.

| Linux | nr | Native Helix | nr |
|-------|----|--------------|----|
| read | 0 | SYSCALL_READ | 20 |
| write | 1 | SYSCALL_WRITE | 21 |
| open / creat | 2 / 85 | SYSCALL_HANDLE_CREATE | 11 |
| close | 3 | SYSCALL_HANDLE_CLOSE | 12 |
| dup / dup2 / dup3 | 32 / 33 / 292 | SYSCALL_HANDLE_DUPLICATE | 13 |
| mmap | 9 | SYSCALL_MAP | 4 |
| munmap | 11 | SYSCALL_UNMAP | 5 |
| brk | 12 | SYSCALL_BRK | 24 |
| sched_yield | 24 | SYSCALL_THREAD_YIELD | 8 |
| clone | 56 | SYSCALL_THREAD_CREATE | 6 |
| execve | 59 | SYSCALL_EXECVE | 25 |
| exit / exit_group | 60 / 231 | SYSCALL_EXIT | 23 |

Not yet mapped (ENOSYS until the kernel grows a native counterpart):
signals, stat, sockets, timers, ioctl, openat-style calls needing an
argument shifter. Native numbers must match the enum in
`kernel/src/syscall.h`; `runtime_test.c` pins them to literals.

## Building and Running

```sh
make              # build runtime binary
make test         # run the translation self-check (39 checks)
make clean        # remove object files and binaries
```

Top-level `make test` runs this self-check after the kernel boot test.
The runtime code is pure freestanding C — no host libc.