# userspace/

Native Helix user-space programs and libraries (planned).

Programs here target the **native object API**: they receive handles to objects
and hold explicit capabilities, rather than Unix file descriptors and a root
user. The first inhabitant will be `init`, the first user-space process, once
the ELF loader and native system-call ABI exist (milestone M9).

Nothing is implemented here yet. See [../ROADMAP.md](../ROADMAP.md).
