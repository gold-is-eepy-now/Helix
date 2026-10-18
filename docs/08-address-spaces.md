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
restores the kernel root, and reclaims the private tables. It needs QEMU
machine testing.

This is the page-table foundation only. Helix does not yet enter ring 3, load
ELF files, provide per-process kernel stacks, or run a process scheduler.
