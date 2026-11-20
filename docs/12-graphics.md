# 12. Graphics

Status: **steps 1–3 built and QEMU-tested.** When virtio-gpu is present, the kernel copies the
GOP screen into a 1280x800 backing buffer, scans it out through virtio-gpu, and flushes only the
changed rectangle after each draw. Without virtio-gpu the GOP framebuffer is used as before.

## Steps

| Step | What | Status |
| --- | --- | --- |
| 1 | virtio 1.0 PCI transport (`virtio.c`): capabilities, 64-bit BARs, feature negotiation, split virtqueue; virtio-gpu `GET_DISPLAY_INFO` (`virtio_gpu.c`); shell command `gpu` | built, QEMU-tested |
| 2 | virtio-gpu 2D resource: `RESOURCE_CREATE_2D`, `RESOURCE_ATTACH_BACKING`, `SET_SCANOUT` | built, QEMU-tested |
| 3 | `TRANSFER_TO_HOST_2D` and `RESOURCE_FLUSH`: draw into a resource, flush the changed rectangle to the screen; a display interface above GOP and virtio | built, QEMU-tested (`fb.c` dirty rectangle, `fb_present`) |
| 4 | Desktop drawing through the new interface, with double buffering | not started |

## Step 1: how the transport works

- The device is found by PCI vendor `0x1AF4` and device `0x1050` (virtio-gpu).
- Vendor capabilities in PCI config space (capability list at offset `0x34`) name a BAR and
  an offset for each structure: common config (status, features, queue addresses), notify,
  and device config (`num_scanouts` at offset 8). BARs may be 64-bit (two dwords).
- Handshake (virtio 1.0 section 3.1.1): reset, ACKNOWLEDGE, DRIVER, read features, accept only
  `VIRTIO_F_VERSION_1`, FEATURES_OK (read it back), set up queues, DRIVER_OK.
- A split virtqueue is three areas in one physically contiguous region: descriptors, the avail
  ring (we post requests here), and the used ring (the device reports completions here).
- One request is outstanding at a time. A request is two descriptors: the command (device
  reads it) and the response (device writes it). We kick the device by writing the queue
  index to the notify address, then poll the used index.
- `GET_DISPLAY_INFO` returns a header and one rectangle per scanout. Step 1 reads them and logs
  them; it does not change the screen.

## Tests

- The shell suite starts QEMU with `-device virtio-gpu-pci` and checks that `gpu` reports
  `gpu: scanout 0: ` (the suite confirms the device answers).
- Checked by hand under QEMU: `gpu` reports `scanout 0: 1280x800`.
- Not tested: real hardware, other virtio-gpu feature bits, error responses from the device.

## Limits of step 1

- One request at a time, polled, with a two-second timeout. No interrupts.
- Only the control queue is set up. The cursor queue is not used yet.
- Drawing goes through virtio only when virtio-gpu is present and the GOP format is 32-bit B8G8R8X8.

## Desktop redesign, slice 2 (0.5.58, QEMU-tested only)

- **Themes:** `theme_t` holds every colour the desktop uses. Five palettes: Helix Aero Blue (default), Helix Aero Dark, Helix Classic, Helix Light, Helix Dark. Settings and the `t` key cycle them.
- **Wallpapers:** `fb_wall_t` is a gradient of 2 to 4 colour stops, laid out vertically or diagonally (`fb_draw_wallpaper_spec`, in `fb.c`). Integer arithmetic only, cached until the screen size or the spec changes. Settings has a Wallpaper button and the `w` key. There are 13 entries: Theme default (the theme's own two colours, the original look), then a Bright and a Dark variant of Sunset, Aurora, Dusk, Ocean, Forest and Candy. Dusk, Ocean and Candy are diagonal; Ocean and Candy use three colour stops. The choice is in memory, so it resets at boot.
- **Taskbar:** one button per window, including minimised ones. The focused or hovered button is lit, and a dot shows that the app is running. The tray glyphs are drawn only. A thin strip at the far right is Show Desktop (hides every window, or restores them all).
- **Start menu:** two columns. The programs column has Pinned, Recent and All Programs, plus a search box that filters by name. The places column has My Computer, Network, Settings, About Helix, and Shut Down. Arrow keys move within a column and switch columns with Left and Right. Typing searches; Esc closes.
- **Desktop menu:** right-click on empty desktop opens Refresh, Settings and About Helix. Any key or click closes it.
- **Windows:** minimise, maximise and restore buttons in the title bar. Maximise fills the screen above the taskbar. Each window casts a solid two-layer shadow.
- **Not done:** blur and translucency (no backbuffer), User and Documents places, window resizing, glossy application icons, and per-application styling beyond the window chrome.
- **Checked on screen:** `tests/wallpaper-shots.py` boots the image under QEMU without virtio-gpu, signs in, opens Settings and presses `w` through all 13 entries, taking a screendump of each. The photos are in `build/wallpapers/` (one PNG per entry plus `contact-sheet.png`); each Settings window names the wallpaper shown. The accelerator is whatever QEMU chooses by default (software emulation here), and the run is not a test on real hardware. With virtio-gpu present, screendumps show only the boot log, so the photos do not cover the virtio display path.
- **Checked:** QEMU screendumps of each theme, the Start menu, the desktop menu, minimise, maximise and Show Desktop. The shell suite (180 checks), USB suite (36/36) and HID unit tests pass.
- **Window controls, 0.5.59 (QEMU-tested only):** double-clicking a title bar within 500 ms toggles maximise. The bottom-right grip resizes a window (minimum 200x120) with an outline, like the move. Alt+Tab focuses the next shown window; Alt+F4 closes the focused one. The keyboard driver (`keyboard.c`) turns these into `SHELL_KEY_ALT_TAB` and `SHELL_KEY_ALT_F4`. USB keyboards do not send these yet. Alt+Tab has no switcher overlay yet.

## Terminal scrollbar (0.5.72, QEMU-tested only)

- The Terminal window runs its scrollback through the shared scrollbar API
  (`kernel/src/scrollbar.c`): `scroll_state_t g_term_scroll`, one bar painted by
  `scrollbar_paint` down the right edge of the text area, clicks read by
  `term_bar_click` with `scrollbar_geometry`/`scrollbar_click`, and Up/Down on the
  keyboard step one row (`scroll_rows`).
- The Terminal starts following the output. Scrolling up (or clicking) leaves the follow,
  so new output does not yank the view down; the next Enter and every typed character jump
  back to the end. Arrow keys do not scroll while the view is at the end — they are not
  command history in this shell yet.
- The terminal window is 480x330 by default: 24 rows of 12 px, a 38-line buffer, so the
  thumb covers 164 of the 260 px of track.
- Checked under QEMU (`/tmp/opencode/term-scrollbar-check.py`): `log` fills the pane,
  Up x10 moves the thumb 69 px up and the text with it, a click on the track pages down
  and clamps back to the bottom. Screenshots were taken with `-vga virtio`; the shell
  suite's own `screendump` (`-vga std` plus `-device virtio-gpu-pci`) shows only the
  kernel's dark boot page, so it was not usable for this check.
- Not done: dragging the thumb, PageUp/PageDown (the keyboard driver has no such codes),
  mouse-wheel scrolling, the Text Editor and the explorer list.
