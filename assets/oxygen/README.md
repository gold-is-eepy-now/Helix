# Oxygen icons (attribution)

These five 32x32 icons are from the Oxygen icon theme, by the KDE community
(https://kde.org, source: https://github.com/KDE/oxygen-icons, commit 3c9f3ef1).

- Files: `32x32/computer.png`, `32x32/help-about.png`, `32x32/utilities-terminal.png`,
  `32x32/accessories-text-editor.png`, `32x32/internet-web-browser.png`.
- License: GNU LGPL version 3 or later (see `LICENSE-LGPL-3.0-or-later.txt`), as
  stated in the upstream `REUSE.toml` for `32x32/**`.
- Copyright: the Oxygen icon authors listed in the upstream `REUSE.toml`.
- The upstream repository ships these icons as PNG only; no SVG source exists for
  them in that tree, so the PNGs are the source files here.

Helix packs these into one file, `oxygen-32.pak`, that the boot loader passes to the
kernel as a module (see `tools/gen-oxygen-icons.py`). They are not compiled into the
kernel binary.
