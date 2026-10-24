# FreeDoom game data

Helix's default Doom IWAD is `freedoom1.wad` from FreeDoom 0.13.0 (Phase 1). If a matching `freedoom1.wad` is in the repository root, the image build uses that local copy; otherwise it downloads the pinned release archive. Game-data WADs are ignored by Git and are not committed. The release archive is pinned by SHA-256 in the top-level `Makefile`; the image includes this directory's `COPYING.adoc` beside the WAD to satisfy the BSD-3-Clause binary redistribution notice.

FreeDoom is a separately licensed game-data project, distinct from the GPL-2.0-or-later DoomGeneric engine. FreeDoom describes Phase 1 as compatible with Doom 1 and permits redistribution when its license notice is retained. See the [official project](https://github.com/freedoom/freedoom), [0.13.0 release](https://github.com/freedoom/freedoom/releases/tag/v0.13.0), and [license](https://github.com/freedoom/freedoom/blob/v0.13.0/COPYING.adoc).

The asset is installed in the boot image as `/boot/doom/doom1.wad`, which lets the existing Limine module handoff pass it to the kernel.

This is boot-data groundwork only. Helix does not launch the Doom engine yet; a playable port is deferred until after M9 userland and will run as a userspace program.
