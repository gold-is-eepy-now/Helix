# DoomGeneric source and Helix port status

The vendored files in this directory come from [ozkl/doomgeneric](https://github.com/ozkl/doomgeneric), upstream revision `e257a09527062bfb986b6e5e0780839b6cbd5b71` (2026-10-18 checkout). The upstream `LICENSE` applies to this code. DoomGeneric is GPL-2.0-or-later; Helix has permission from its maintainer to include/link this engine.

This is a source import and boot-data experiment, not a working game port. The current image can pass an IWAD through Limine, and Helix has framebuffer presentation and PS/2 key events. Doom itself is intentionally deferred until after M9 creates a userland executable/runtime path; the engine must run as a userspace program rather than being linked into the kernel. See `../../docs/07-doom-port.md` for the sequencing decision and `../freedoom/README.md` for the asset license.

The regular build stages FreeDoom Phase 1 as a boot module. To use another IWAD instead, build with:

```sh
make DOOM_WAD=/path/to/doom1.wad
```

This places the selected IWAD at `/boot/doom/doom1.wad` and registers it as a Limine module. Game-data WADs are ignored by Git; the build uses a root `freedoom1.wad` when present or downloads the pinned release into ignored build dependencies.
