# Optional Linux Kernel Drivers

This directory holds optional drivers pulled from the `third-party/linux`
git submodule. They are never built by default — Helix ships without them.
Build them explicitly with `make linux-drivers`.

The submodule is an *optional* dependency: if `third-party/linux` is empty
(the default state — out of the box), the rest of the project builds cleanly.
Cloning it requires a network access to the kernel source tree.

## Why here

Linux network/graphics/sound driver sources are heavy and x86-specific.
The Helix kernel already has native networking, display and input object
frameworks (M12). These options layer Linux driver functionality on top of
that existing stack, assembled from upstream sources rather than written from
scratch. Keeping them in this optional-drivers tree keeps them off the main
kernel branch's contribution path and explicit-testable from the userspace.

## Driver categories

| Category | Drivers | Source location (inside `third-party/linux`) |
|----------|---------|----------------------------------------------|
| Ethernet | RealTek (`r815x`, `r8120`) / Intel (`e1000`, `igb`) | `drivers/staging/media/ethernet/realtek/` / `drivers/net/` |
| WiFi     | Intel (`iwlwifi`) / RealTek (`rtlwifi`) / FOSS Broadcom (`brcmfmac`, `ath11k`) | `drivers/net/wireless/intel/` / `rtlwifi/` / `iwlwifi/` / `brcmfmac/`, `ath11k/` |
| Graphics | Mesa (`drm/mesa`) / NVIDIA Zink (`drivers/gpu/drm/zink/`) | `drivers/gpu/drm/mesa/` / `drivers/gpu/drm/zink/` |
| Sound    | Intel HDA (`snd_hdac_intel`) / Realtek (`allegro`, `hdmi`) | `sound/core/hda/` / `sound/pci/hda/` |

## Per-driver options

Each optional driver lives under its own subdirectory and is built when you
run the matching `make linux-drivers:<device>` target:

```sh
make linux-drivers:ethernet   # RealTek + Intel Ethernet
make linux-drivers:wifi        # Intel + RealTek + FOSS Broadcom WiFi
make linux-drivers:graphics    # Mesa + NVK/Zinc (NVIDIA)
make linux-drivers:sound       # Intel HDA + Realtek Codec
make linux-drivers             # all of the above at once
```

If `third-party/linux` is not initialized, these targets fail with a clear
message and exit 1 rather than silently building nothing.
