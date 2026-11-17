# MEL-DIFF — MelDev vs Dev

This file tracks how the **MelDev** branch differs from **Dev**, and keeps a
dated journal of changes made on MelDev. Newest entries are at the top of the
journal.

**How this was measured:** `git diff --stat origin/Dev...MelDev` on
2026-11-15 — merge base `1eb6931`, `Dev` is an ancestor of `MelDev`
(0 commits on Dev that MelDev lacks), MelDev is 74 commits ahead,
**178 files changed, 25,487 insertions(+), 184 deletions(-)**.

---

## Differences from Dev

### Identity

| Area | Dev | MelDev |
|------|-----|--------|
| Version string | `0.5.18-dev.20261110` | `0.5.71-dev.20261117.MELDEV` |
| Codename | `DoneJS` | `Platter` |
| Build date | 2026-11-10 | 2026-11-17 |
| Build string | `dev.20261110` | `dev.20261117` + `.MELDEV` suffix (MelDev marker) |
| Compile-and-boot rule in CLAUDE.md | Required | Waived on MelDev (experiment branch); the honesty rule still applies |
| Branch-only files | — | `MEL-DIFF.md`, `ABOUT-MELDEV.md`, `userspace/PORT-AUDIT.md` |

### Subsystems present on MelDev and absent on Dev

Every file below is an **addition** (`git diff --name-status` shows `A`) unless
noted. All are QEMU-tested unless the row says otherwise.

| Area | Files | State |
|------|-------|-------|
| Graphical desktop | `kernel/src/desktop.c` (3,156 lines), `desktop.h`, `editor.c`, `html_text.c`, `icon_pack.c`, `login.c`, `accounts.c`, `ps2_mouse.c` | Windows-7-style desktop, sign-in screen, five themes, Explorer-style My Computer, Browser/Terminal/Editor windows |
| Display/input hardware | `kernel/src/fb.c` (modified, +270), `input.c`, `keyboard.c` | Framebuffer desktop path, PS/2 mouse |
| virtio-gpu | `kernel/src/virtio.c`, `virtio_gpu.c` (modified) | Display list read; desktop still draws to the GOP framebuffer |
| Network stack | `net.c`, `net_arp.c`, `net_ethernet.c`, `net_ip.c`, `net_icmp.c`, `net_udp.c`, `net_dns.c`, `net_dhcp.c`, `net_tcp.c`, `net_http.c`, `net_interface.c`, `syscall_net.c`, `e1000.c` | e1000 + Ethernet/ARP/IPv4/ICMP/UDP/DNS/DHCP/TCP/HTTP, QEMU-tested |
| TLS 1.3 + PKI | `tls13.c`, `tls13_client.c`, `tls13_hello.c`, `tls13_record.c`, `x509.c`, `trust_roots.c`, `hkdf.c`, `sha256.c`, `x25519.c`, `chacha20poly1305.c` | Host-tested against RFC vectors; no HTTPS in the browser yet |
| Storage | `ahci.c`, `block_device.c`, `ext4.c`, `ufile.c` (modified), `syscall_fs.c` | Read-only SATA (`disk`), ext4 root file commands |
| ACPI / PCI | `acpi.c`, `commands.c` (`acpi`, `pci`) | Tables read and checksummed; no AML interpreter |
| Apple SPI / top-case / T2 | `spi.c`, `intel_lpss_gspi.c`, `apple_topcase_spi.c`, `t2.c`, `t2_protocol.c` | Controller detect + init; T2 mailbox handshake, `t2info` |
| xHCI / USB listing | `xhci.c`, `usb_host.c`, `usb_hid.c` (modified) | Read-only discovery; HID keyboard/mouse |
| Kernel event log | `klog.c` | 512-event ring, dumped on a CPU exception |
| Accounts | `accounts.c`, `login.c` | `/etc/users`, PBKDF2 hashes, `mkusr`/`su`/`passwd` |
| Icons | `assets/oxygen/*` (5 PNGs + licence), `tools/gen-oxygen-icons.py` | Built into `oxygen-32.pak` without Pillow |
| New tests | `tests/t2-unit.c`, `tests/tls13-*.c`, `tests/x509-unit.c`, `tests/hkdf-unit.c`, `tests/crypto-unit.c`, `tests/ext4-test.c` | Hosted, run by `make test` |
| New docs | `docs/10-network.md`, `12-graphics.md`, `16-acpi.md`, `17-spi.md`, `18-topcase.md`, `19-xhci.md`, `20-ahci.md` | Subsystem documents |

### Net removals / behavioural differences

- `kernel/src/pci.h`: `pci_find_device()` takes a `size_t index` argument on
  MelDev; Dev's form has no index.
- `Makefile`: MelDev adds the `t2-unit` test and passes `CC="$(HOST_CC)"` into
  the `runtime`, `mac-darwin-compat` and `donejs-compat` hosted suites.
- `docs/02-shell.md`: MelDev documents the `t2info` command.

---

## Journal

### 2026-11-17

| Time (UTC) | Change | Commit | Notes |
|------------|--------|--------|-------|
| 11:25 | Scrollbar API (`kernel/src/scrollbar.c`): the Browser's bar now takes its geometry and click handling from `scrollbar_geometry`/`scrollbar_click`; bumped to `0.5.70-dev.20261117.MELDEV` | _this commit_ | Host test `tests/scrollbar-unit.c` (22 checks) in `make test`. Thumb dragging and a bar for the editor/explorer lists are not done. |
| 11:15 | Re-merged `origin/desktop-test`: brought in the read-only FAT32 driver and USB mass storage (2 commits MelDev was still missing); resolved Makefile, `docs/02-shell.md` and `version.h` by hand | [`b40275b`](https://github.com/gold-is-eepy-now/Helix/commit/b40275b) | `make test` rc=0: shell-test 166 checks, fat32-test 110 checks, USB, HID, T2, browser-unit. |
| 13:20 | Docs: banner and sysinfo samples moved to `0.5.71-dev.20261117.MELDEV` | [`915a7d2`](https://github.com/gold-is-eepy-now/Helix/commit/915a7d2) | Docs only; code version already `0.5.71-dev.20261117.MELDEV`. |
| 12:34 | Explorer opens text files in the Text Editor (`is_text_name` + `exp_open` routing); binary programs and other file kinds still preview only | [`acc78a5`](https://github.com/gold-is-eepy-now/Helix/commit/acc78a5) | TODO item 6, part 1. |
| 11:00 | Browser hyperlinks and pictures (TODO item 8): `html_to_text_ex()` numbers `<a href>` and records `<img src>`/alt; the Browser follows a link with Alt+L plus digits or a click on its row, and draws fetched BMP/PPM pictures (new `kernel/src/image.c`) | [`8bb3f85`](https://github.com/gold-is-eepy-now/Helix/commit/8bb3f85) | Host unit test `tests/browser-unit.c` (37 checks) wired into `make test`. Browser rendering not yet checked against a live page in QEMU. |
| 10:38 | Version identity set to `0.5.67-dev.20261117.MELDEV`, build date `2026-11-17` | [`b79eceb`](https://github.com/gold-is-eepy-now/Helix/commit/b79eceb) | Date follows the requested ETA of 17 November; codename stays `Platter`. |

### 2026-11-15

| Time (UTC) | Change | Commit | Notes |
|------------|--------|--------|-------|
| 23:51 | Resolved the seven conflicted files of the desktop-test merge in favour of the MelDev side (CLAUDE.md, Makefile, docs/02-shell.md, kernel/src/pci.{c,h}, kernel/src/intel_lpss_gspi.c, kernel/src/virtio.c, kernel/src/version.h); dropped the stale index-less `pci_find_device()` prototype | [`f17d5cb`](https://github.com/gold-is-eepy-now/Helix/commit/f17d5cb) | Merge completed. `make test` exits 0 (shell, USB, HID, t2, tls, x509, donejs, darwin suites). |
| 23:51 | Confirmed TODO item 1: version string is `0.5.65-dev.20261115.MELDEV`, build date `2026-11-15` | _in tree_ | `.MELDEV` suffix identifies the branch. |
| 23:51 | Confirmed TODO item 2: `origin/desktop-test` = `91e8f32`, already an ancestor of MelDev (merge commit `9efe684`); nothing left to merge | _in tree_ | Merge existed but shipped conflict markers; now cleaned. |
| 23:58 | Added the shell-scripting work to the journal: `sh <file>`, `export`/`unset`/`$VAR`, `if`/`then`/`else`/`fi`, `test` | [`4c4847c`](https://github.com/gold-is-eepy-now/Helix/commit/4c4847c) | New kernel module `kernel/src/shell_script.c`; machine-tested via `tests/shell-test.py`. |

| 19:02 | Merge of `origin/desktop-test` committed **with conflict markers still present** in 8 files | [`9efe684`](https://github.com/gold-is-eepy-now/Helix/commit/9efe684) | Fixed by `f17d5cb` above. |
| 04:05 | Bumped version to `0.5.65-dev.20261115.MELDEV` in `kernel/src/version.h` | [`e1f2047`](https://github.com/gold-is-eepy-now/Helix/commit/e1f2047) | Also brought in 175 files (25,413 insertions) from the desktop-test line. |
| 02:10 | Bumped version to `0.5.20-dev.20261115.MELDEV` (first Nov-15 attempt) | [`4a022a8`](https://github.com/gold-is-eepy-now/Helix/commit/4a022a8) | Superseded by `0.5.65-dev.20261115.MELDEV`. |
| 01:20 | Merged MelDev through PR #5 | [`8d35fda`](https://github.com/gold-is-eepy-now/Helix/commit/8d35fda) | GitHub merge of the MelDev line into itself. |
| 01:19 | Added the userspace CLI port audit (`userspace/PORT-AUDIT.md`) | [`e06150b`](https://github.com/gold-is-eepy-now/Helix/commit/e06150b) | TODO item 3, audit only — nothing built or booted. |
| 00:29 | Journaled the `CLAUDE.md` MelDev exception in this file | [`885cac9`](https://github.com/gold-is-eepy-now/Helix/commit/885cac9) | Documentation only. |
| 00:29 | Added the MelDev branch exception to `CLAUDE.md` | [`731a7bc`](https://github.com/gold-is-eepy-now/Helix/commit/731a7bc) | Documentation only. |

### 2026-11-14

| Time (UTC) | Change | Commit | Notes |
|------------|--------|--------|-------|
| 23:54 | Bumped version to `0.5.19-dev.20261114.MELDEV` in `kernel/src/version.h` | [`602f512`](https://github.com/gold-is-eepy-now/Helix/commit/602f512) | Superseded. |
| 23:54 | Created this file (`MEL-DIFF.md`) | [`caa3c29`](https://github.com/gold-is-eepy-now/Helix/commit/caa3c29) | Journal started. |
| 23:55 | Corrected the timestamp on the journal entry for `MEL-DIFF.md` | [`c39fe43`](https://github.com/gold-is-eepy-now/Helix/commit/c39fe43) | Documentation only. |
