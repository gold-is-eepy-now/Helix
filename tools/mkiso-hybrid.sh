#!/bin/sh
# mkiso-hybrid.sh — build a hybrid image: one file that boots from BIOS and UEFI, from a
# CD/DVD or from a USB stick written with dd.
#
# Two El Torito entries (BIOS and UEFI) are made by xorriso, then Limine's own
# `limine bios-install` writes an MBR into the image, so it also works as a raw disk.
#
# Requires the `limine` tool built from limine.c in LIMINE_DIR (cc -O2 -o limine limine.c).
# Environment overrides: BUILD_DIR KERNEL LIMINE_DIR ISO LIMINE_CONF
set -eu

PATH="$PATH:/usr/sbin:/sbin"
export PATH

BUILD_DIR="${BUILD_DIR:-build}"
KERNEL="${KERNEL:-$BUILD_DIR/kernel}"
LIMINE_DIR="${LIMINE_DIR:-$BUILD_DIR/deps/limine-binary}"
ISO="${ISO:-$BUILD_DIR/helix-hybrid.iso}"
LIMINE_CONF="${LIMINE_CONF:-limine.conf}"
ISO_ROOT="$BUILD_DIR/iso_root_hybrid"

for t in xorriso; do
    command -v "$t" >/dev/null 2>&1 || { echo "mkiso-hybrid: $t not found" >&2; exit 1; }
done
[ -x "$LIMINE_DIR/limine" ] || { echo "mkiso-hybrid: limine tool not found in $LIMINE_DIR (build it from limine.c)" >&2; exit 1; }
for f in limine-bios-cd.bin limine-bios.sys limine-uefi-cd.bin BOOTX64.EFI; do
    [ -f "$LIMINE_DIR/$f" ] || { echo "mkiso-hybrid: $f not found in $LIMINE_DIR" >&2; exit 1; }
done
[ -f "$KERNEL" ] || { echo "mkiso-hybrid: kernel not found: $KERNEL" >&2; exit 1; }
[ -f "$LIMINE_CONF" ] || { echo "mkiso-hybrid: limine.conf not found: $LIMINE_CONF" >&2; exit 1; }

echo "[mkiso-hybrid] staging ISO tree in $ISO_ROOT"
rm -rf "$ISO_ROOT"
mkdir -p "$ISO_ROOT/boot/limine" "$ISO_ROOT/EFI/BOOT"
cp "$KERNEL"                              "$ISO_ROOT/boot/kernel"
cp "$LIMINE_CONF"                         "$ISO_ROOT/boot/limine/limine.conf"
cp "$LIMINE_DIR/limine-bios-cd.bin"       "$ISO_ROOT/boot/limine/"
cp "$LIMINE_DIR/limine-bios.sys"          "$ISO_ROOT/boot/limine/"
cp "$LIMINE_DIR/limine-uefi-cd.bin"       "$ISO_ROOT/boot/limine/"
cp "$LIMINE_DIR/BOOTX64.EFI"              "$ISO_ROOT/EFI/BOOT/"

echo "[mkiso-hybrid] building $ISO"
mkdir -p "$(dirname "$ISO")"
# -partition_offset 16 puts the ISO partition at sector 64 (bios-install needs >= 63).
# -append_partition adds an EFI System Partition (type 0xef) holding the UEFI image, so a
# UEFI firmware can boot the stick written with dd; the BIOS path uses the MBR and Limine's stage.
xorriso -as mkisofs -R -r -J \
    -partition_offset 16 \
    -b boot/limine/limine-bios-cd.bin \
    -no-emul-boot -boot-load-size 4 -boot-info-table \
    -eltorito-alt-boot \
    -e boot/limine/limine-uefi-cd.bin \
    -no-emul-boot \
    -append_partition 2 0xef "$LIMINE_DIR/limine-uefi-cd.bin" \
    "$ISO_ROOT" -o "$ISO" >/dev/null

echo "[mkiso-hybrid] writing BIOS MBR (limine bios-install)"
"$LIMINE_DIR/limine" bios-install "$ISO"

rm -rf "$ISO_ROOT"
echo "[mkiso-hybrid] done: $ISO"
