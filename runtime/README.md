# runtime/

User-space runtimes (planned).

This is where the **native runtime** (a minimal libc-style support layer for
Helix programs) will live, and — much later — the compatibility runtimes:

- **POSIX layer** — maps file descriptors, `fork`/`exec` and signals onto
  Helix handles, objects and channels.
- **Linux runtime** — translates Linux system calls into the native object API.
- **BSD runtime** — translates BSD system calls into the native object API.

These runtimes sit **above** the kernel. They translate Unix behaviour into
Helix's native model; they do not change the kernel's native design.

Nothing is implemented here yet. See [../ROADMAP.md](../ROADMAP.md).
