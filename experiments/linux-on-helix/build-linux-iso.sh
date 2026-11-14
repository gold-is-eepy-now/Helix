#!/bin/sh
# build-linux-iso.sh — make a bootable UEFI ISO that boots a Linux kernel through Limine.
#
# Usage: build-linux-iso.sh BZIMAGE OUT_ISO
#
# Limine (the bootloader Helix uses) loads BZIMAGE with the Linux boot protocol,
# and passes it an initramfs that holds the static /init from init/init.c.
# Helix's own kernel is not in this ISO.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
LIMINE_DIR="${LIMINE_DIR:-$REPO/build/deps/limine-binary}"

[ $# -eq 2 ] || { echo "usage: $0 BZIMAGE OUT_ISO" >&2; exit 2; }
BZIMAGE=$1
OUT=$2
[ -f "$BZIMAGE" ] || { echo "no such kernel image: $BZIMAGE" >&2; exit 1; }
[ -f "$LIMINE_DIR/limine-uefi-cd.bin" ] || { echo "Limine binaries not found in $LIMINE_DIR (run make deps in the repo first)" >&2; exit 1; }
command -v xorriso >/dev/null || { echo "xorriso is required" >&2; exit 1; }
command -v cpio >/dev/null || { echo "cpio is required" >&2; exit 1; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

echo "[linux-iso] building /init (static)"
gcc -static -O2 -Wall -o "$WORK/init" "$HERE/init/init.c"

echo "[linux-iso] packing initramfs"
mkdir -p "$WORK/root"
cp "$WORK/init" "$WORK/root/init"
(cd "$WORK/root" && find . | cpio -o -H newc --quiet) > "$WORK/initramfs.cpio"

echo "[linux-iso] staging ISO tree"
ROOT="$WORK/iso"
mkdir -p "$ROOT/boot/limine" "$ROOT/EFI/BOOT"
cp "$BZIMAGE" "$ROOT/boot/vmlinuz"
cp "$WORK/initramfs.cpio" "$ROOT/boot/initramfs.cpio"
cp "$HERE/limine.conf" "$ROOT/boot/limine/limine.conf"
cp "$LIMINE_DIR/limine-uefi-cd.bin" "$ROOT/boot/limine/"
cp "$LIMINE_DIR/BOOTX64.EFI" "$ROOT/EFI/BOOT/"

echo "[linux-iso] writing $OUT"
mkdir -p "$(dirname "$OUT")"
xorriso -as mkisofs -R -r -J \
    --efi-boot boot/limine/limine-uefi-cd.bin \
    -efi-boot-part --efi-boot-image --protective-msdos-label \
    "$ROOT" -o "$OUT" >/dev/null 2>&1
echo "[linux-iso] done: $OUT"
