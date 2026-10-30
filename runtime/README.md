# runtime/
User-space runtimes (planned).

This is where the native runtime (a minimal libc-style support layer for
Helix programs) lives, and — much later — the compatibility runtimes:

- **POSIX layer** — maps file descriptors, `fork`/`exec` and signals onto
  Helix handles, objects and channels.
- **Linux runtime** — translates Linux system calls into the native object API.
  Provides an ELF loader for Linux binaries and a syscall translation layer
  that maps Linux syscalls to Helix native calls via the object manager,
  handles, and channels.
- **BSD runtime** — translates BSD system calls into the native object API.

These runtimes sit **above** the kernel. They translate Unix behaviour into
Helix's native model; they do not change the kernel's native design.

## Linux Runtime

The Linux runtime is a userspace process that hosts Linux ELF binaries. It
provides:

1. **ELF loader** — loads Linux/BSD ELF binaries, sets up memory mappings,
   and starts execution. Uses the `elf_load()` function to parse ELF64
   headers (magic, class, machine, type) and program headers.
2. **Syscall translation** — intercepts Linux system calls (int $0x80 or
   `syscall` instruction in x86_64 ABI) and maps them to native Helix syscalls
   via the translation table defined in `linux_syscall.c`. Common mappings:
   - Linux read(0, buf, count) -> Helix read(handle, buf, count)
   - Linux write(1, buf, count) -> Helix write(handle, buf, count)
   - Linux exit(status) -> Helix exit(status)
   - Linux brk(addr) -> Helix brk(addr)
3. **File descriptor layer** — maps Linux file descriptors (0=stdin, 1=stdout,
   2=stderr) to Helix file handles via `linux_fd_to_handle()`. Uses the object
   manager for file-backed operations (currently ramdisk-backed).
4. **Minimal libc emulation** — provides basic C library functions
   (`write`, `exit`) translated to native calls.

### Syscall Translation Table

The translation table maps Linux syscall numbers to native Helix syscall
indices:

| Linux | Native Helix | Description |
|-------|-------------|-------------|
| 0     | SYSCALL_READ (20) | read() |
| 1     | SYSCALL_WRITE (21) | write() |
| 60    | SYSCALL_EXIT (23) | exit() |
| 12    | SYSCALL_BRK (24) | brk() |

Unmapped syscalls return ENOSYS (-38). Invalid Linux FDs return EBADF (-9).

### Building and Running

The runtime code is pure C (freestanding, no host libc). It can be compiled
with `make` from the `runtime/` directory:

```sh
make
```

The build produces `linux_runtime` binary. After building the kernel
(`make` in the project root), the runtime can be launched from the Helix
shell or as the init process.

See the individual source files for API details and implementation notes.

## Building and Running

```sh
make              # build linux_runtime in runtime/
make clean        # remove object files and binary
```