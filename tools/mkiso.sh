#!/bin/sh
# mkiso.sh — build a bootable UEFI ISO (El Torito) from the kernel.
#
# The ISO contains:
#   /EFI/BOOT/BOOTX64.EFI         Limine's UEFI bootloader
#   /boot/limine/limine-uefi-cd.bin  the El Torito EFI boot image
#   /boot/limine/limine.conf      what to boot
#   /boot/kernel                  our kernel ELF
#
# Limine reads limine.conf and the kernel straight from the ISO9660 filesystem,
# so this works as a CD/DVD in QEMU (`-cdrom`), VirtualBox (attach as an
# optical drive with EFI firmware), and on real hardware.
#
# Environment overrides: BUILD_DIR KERNEL LIMINE_DIR ISO LIMINE_CONF
set -eu

PATH="$PATH:/usr/sbin:/sbin"
export PATH

BUILD_DIR="${BUILD_DIR:-build}"
KERNEL="${KERNEL:-$BUILD_DIR/kernel}"
LIMINE_DIR="${LIMINE_DIR:-$BUILD_DIR/deps/limine-binary}"
ISO="${ISO:-$BUILD_DIR/helix.iso}"
LIMINE_CONF="${LIMINE_CONF:-limine.conf}"
ISO_ROOT="$BUILD_DIR/iso_root"

command -v xorriso >/dev/null 2>&1 || {
    echo "mkiso: xorriso not found (Debian/Ubuntu: 'sudo apt install xorriso'; macOS: 'brew install xorriso')" >&2
    exit 1
}

[ -f "$KERNEL" ] || { echo "mkiso: kernel not found: $KERNEL" >&2; exit 1; }
[ -f "$LIMINE_DIR/limine-uefi-cd.bin" ] || { echo "mkiso: limine-uefi-cd.bin not found in $LIMINE_DIR" >&2; exit 1; }
[ -f "$LIMINE_DIR/BOOTX64.EFI" ] || { echo "mkiso: BOOTX64.EFI not found in $LIMINE_DIR" >&2; exit 1; }
[ -f "$LIMINE_CONF" ] || { echo "mkiso: limine.conf not found: $LIMINE_CONF" >&2; exit 1; }

echo "[mkiso] staging ISO tree in $ISO_ROOT"
rm -rf "$ISO_ROOT"
mkdir -p "$ISO_ROOT/boot/limine" "$ISO_ROOT/EFI/BOOT"
cp "$KERNEL"                              "$ISO_ROOT/boot/kernel"
cp "$LIMINE_CONF"                         "$ISO_ROOT/boot/limine/limine.conf"
cp "$LIMINE_DIR/limine-uefi-cd.bin"       "$ISO_ROOT/boot/limine/"
cp "$LIMINE_DIR/BOOTX64.EFI"              "$ISO_ROOT/EFI/BOOT/"

echo "[mkiso] building $ISO"
mkdir -p "$(dirname "$ISO")"
xorriso -as mkisofs -R -r -J \
    --efi-boot boot/limine/limine-uefi-cd.bin \
    -efi-boot-part --efi-boot-image --protective-msdos-label \
    "$ISO_ROOT" -o "$ISO" >/dev/null

rm -rf "$ISO_ROOT"
echo "[mkiso] done: $ISO"
