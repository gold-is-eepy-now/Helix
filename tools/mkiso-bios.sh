#!/bin/sh
# mkiso-bios.sh — build a legacy BIOS bootable ISO (El Torito) from the kernel.
#
# This is the legacy-BIOS counterpart of mkiso.sh. The kernel and limine.conf are
# the same; only the boot media differs. Limine's BIOS stage (limine-bios-cd.bin)
# is the El Torito boot image, and it loads limine-bios.sys from the ISO.
#
# The ISO contains:
#   /boot/limine/limine-bios-cd.bin  the El Torito BIOS boot image (no emulation)
#   /boot/limine/limine-bios.sys     Limine's BIOS stage 3, loaded by the stage above
#   /boot/limine/limine.conf         what to boot
#   /boot/kernel                     our kernel ELF
#
# Environment overrides: BUILD_DIR KERNEL LIMINE_DIR ISO LIMINE_CONF
set -eu

PATH="$PATH:/usr/sbin:/sbin"
export PATH

BUILD_DIR="${BUILD_DIR:-build}"
KERNEL="${KERNEL:-$BUILD_DIR/kernel}"
LIMINE_DIR="${LIMINE_DIR:-$BUILD_DIR/deps/limine-binary}"
ISO="${ISO:-$BUILD_DIR/helix-bios.iso}"
LIMINE_CONF="${LIMINE_CONF:-limine.conf}"
ISO_ROOT="$BUILD_DIR/iso_root_bios"

command -v xorriso >/dev/null 2>&1 || {
    echo "mkiso-bios: xorriso not found (Debian/Ubuntu: 'sudo apt install xorriso'; macOS: 'brew install xorriso')" >&2
    exit 1
}

[ -f "$KERNEL" ] || { echo "mkiso-bios: kernel not found: $KERNEL" >&2; exit 1; }
[ -f "$LIMINE_DIR/limine-bios-cd.bin" ] || { echo "mkiso-bios: limine-bios-cd.bin not found in $LIMINE_DIR" >&2; exit 1; }
[ -f "$LIMINE_DIR/limine-bios.sys" ] || { echo "mkiso-bios: limine-bios.sys not found in $LIMINE_DIR" >&2; exit 1; }
[ -f "$LIMINE_CONF" ] || { echo "mkiso-bios: limine.conf not found: $LIMINE_CONF" >&2; exit 1; }

echo "[mkiso-bios] staging ISO tree in $ISO_ROOT"
rm -rf "$ISO_ROOT"
mkdir -p "$ISO_ROOT/boot/limine"
cp "$KERNEL"                              "$ISO_ROOT/boot/kernel"
cp "$LIMINE_CONF"                         "$ISO_ROOT/boot/limine/limine.conf"
cp "$LIMINE_DIR/limine-bios-cd.bin"       "$ISO_ROOT/boot/limine/"
cp "$LIMINE_DIR/limine-bios.sys"          "$ISO_ROOT/boot/limine/"

echo "[mkiso-bios] building $ISO"
mkdir -p "$(dirname "$ISO")"
xorriso -as mkisofs -R -r -J \
    -b boot/limine/limine-bios-cd.bin \
    -no-emul-boot -boot-load-size 4 -boot-info-table \
    "$ISO_ROOT" -o "$ISO" >/dev/null

rm -rf "$ISO_ROOT"
echo "[mkiso-bios] done: $ISO"
