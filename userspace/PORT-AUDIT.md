# Userspace CLI port audit (TODO item 3)

Status of each program in `userspace/apps/`, checked against the native
`libhelix` API (`userspace/libhelix.h`). Nothing here has been built or booted.

| App | Kind | Status | Blocker |
|-----|------|--------|---------|
| `hello.c` | CLI | Real, uses only `stdio.h` | None. Already portable. |
| `volume.c` | CLI | **Stub**: prints fixed values (50%) and never touches a sound device | No sound-device syscall in `libhelix` |
| `wget.c` | CLI | **Stub**: prints "Simulate HTTP GET" and "download complete" | No socket syscalls in `libhelix` (the `desktop-test` branch has an HTTP client, not yet merged) |
| `dhcp.c`, `wifi.c`, `wav.c`, `helix_test.c` | CLI | Not yet audited | - |
| `boxwm.c`, `browser.c`, `terminal.c`, `start-gui.c`, `x11server.c` | X / GUI | Not yet audited | - |

## Notes

- Stubs are not ported as working features. A port of a stub must say it is a
  stub in its output and in this table.
- Real network and sound behaviour needs new syscalls first. Those belong in
  the kernel milestone for each subsystem, not in the userspace port.
