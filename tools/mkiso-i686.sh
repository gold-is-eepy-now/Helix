#!/bin/sh
# mkiso-i686.sh — legacy BIOS ISO for the i686 kernel, booted by GRUB (Multiboot 1).
#
# Why GRUB and not Limine: Limine loads only x86-64 kernels (tested on its BIOS and IA-32
# UEFI loaders; both reject ELF32). GRUB's i386-pc loader boots Multiboot ELF32 kernels.
#
# Environment overrides: BUILD_DIR KERNEL ISO
set -eu

PATH="$PATH:/usr/sbin:/sbin"
export PATH

BUILD_DIR="${BUILD_DIR:-build}"
KERNEL="${KERNEL:-$BUILD_DIR/i686/kernel.elf}"
ISO="${ISO:-$BUILD_DIR/i686/helix-i686-bios.iso}"
WORK="$BUILD_DIR/i686/iso_work"

for t in grub-mkrescue xorriso; do
    command -v "$t" >/dev/null 2>&1 || { echo "mkiso-i686: $t not found (Debian/Ubuntu: 'sudo apt install grub-pc-bin grub-common xorriso mtools')" >&2; exit 1; }
done
[ -f "$KERNEL" ] || { echo "mkiso-i686: kernel not found: $KERNEL" >&2; exit 1; }
grub-file --is-x86-multiboot "$KERNEL" || { echo "mkiso-i686: $KERNEL has no Multiboot header" >&2; exit 1; }

rm -rf "$WORK" && mkdir -p "$WORK/tree/boot/grub"
cp "$KERNEL" "$WORK/tree/boot/kernel.elf"
cat > "$WORK/tree/boot/grub/grub.cfg" <<'CFG'
set timeout=0
set default=0
menuentry "Helix OS (i686)" {
    multiboot /boot/kernel.elf
    boot
}
CFG

mkdir -p "$(dirname "$ISO")"
grub-mkrescue -o "$ISO" "$WORK/tree" >/dev/null 2>&1
rm -rf "$WORK"
echo "[mkiso-i686] done: $ISO"
