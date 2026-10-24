# M9: User address spaces

M9 starts by giving each future process its own lower-half page-table root.
The new root copies the kernel's upper-half PML4 entries, which point to the
same supervisor-only kernel mappings in every address space. Its lower half
starts empty and receives user mappings independently.

The current API is in `kernel/src/vmm.h`:

- `vmm_address_space_create()` creates a root and shares the kernel mappings.
- `vmm_address_space_map_user()` maps one aligned lower-half page with the
  hardware USER bit set through every required page-table level.
- `vmm_address_space_switch()` loads the root into CR3.
- `vmm_address_space_get_mapping()` and
  `vmm_address_space_unmap_user()` inspect and remove user mappings.
- `vmm_address_space_destroy()` frees the private page-table tree. The caller
  still owns and must release mapped data frames.

`asptest` creates a space, maps a writable NX user page, switches CR3, checks
that kernel code remains supervisor-only, writes through the user mapping,
restores the kernel root, and reclaims the private tables. This self-test has
passed in QEMU.

Isolated page-table roots and the initial ring-3 entry/return diagnostic have
passed in QEMU. General syscalls, process-specific kernel stacks, and a user
process scheduler are still ahead.

## Privilege-transition groundwork

Helix now installs its own flat long-mode GDT with kernel and DPL-3 code/data
descriptors, plus a 64-bit TSS. The TSS `RSP0` initially points at the guarded
kernel stack; `usermodetest` switches it to a separate guarded transition
stack. The GDT load also reloads CS and the data selectors before the IDT is
installed. Startup has been confirmed in QEMU.

`usermodetest` maps a tiny machine-code program and a user stack, enters ring 3,
and returns through the DPL-3 `int 0x80` exit syscall. Its kernel trampoline
restores the calling kernel stack. This passed in QEMU. A new `usermodetest`
case checks that a ring-3 invalid-opcode exception returns to the kernel shell
instead of panicking; exception 6 passed this check in QEMU. Interrupts stay
disabled during the run, and the transition stack
must become scheduler-managed before concurrent user processes can run.

## Native ELF loading foundation

`elf64_load()` accepts static x86-64 ELF64 `ET_EXEC` images. It validates the
header and program-header bounds, maps `PT_LOAD` pages into an existing user
address space, zeroes memory beyond each segment's file size, and applies
write/execute permissions after copying. It rejects dynamic/interpreted
binaries and writable-executable segments. The caller owns the address space
and can release image frames with `elf64_unload()`.

The image now carries static `.hlx` ELF modules through Limine. The `runapp`
shell command maps the selected module into a private address space, provides
a user stack, enters ring 3, and reports its exit status or contained fault.
With no filename, it launches the first app; `runapp shell.hlx` selects the
interactive user shell. `.hlx` is a Helix app extension for native ELF64, not
a separate binary encoding.

Build the image with the cross compiler and boot it. Helix starts `shell.hlx`
as the initial user process, and returns to the kernel maintenance shell after
the user shell exits. `runapp` launches the hello app; `runapp shell.hlx` starts
the user shell manually. Boot startup and fallback passed in QEMU. The initial
write and bounded line-read syscalls are documented in
[docs/09-syscalls.md](09-syscalls.md). Apps are still supplied as boot modules;
they cannot yet read files or be discovered from a filesystem. The planned
native filesystem is HFS (Helix Filesystem), tracked under M11.
