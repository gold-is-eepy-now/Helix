#!/bin/sh
# mkimage.sh — assemble a bootable UEFI disk image.
#
# Layout produced (a 64 MiB GPT disk by default):
#
#   sector 2048 .......................... EFI System Partition (FAT32)
#       /EFI/BOOT/BOOTX64.EFI   the Limine UEFI bootloader
#       /boot/limine/limine.conf  what to boot
#       /boot/kernel              our kernel ELF
#
# The firmware (OVMF in QEMU, or real UEFI on hardware) reads
# /EFI/BOOT/BOOTX64.EFI from the ESP with no configuration at all — that is
# the UEFI "removable media" boot path. Limine then reads limine.conf.
#
# The disk is UEFI-only: a pure GPT with a standard protective MBR and a
# FAT32 EFI System Partition. We deliberately do NOT create a hybrid MBR
# (the old `sgdisk -m 1`), because that only helps legacy BIOS booting --
# which Helix does not support -- and it makes some firmware (including
# VirtualBox's EFI) misread the partition table.
#
# Environment overrides: BUILD_DIR KERNEL LIMINE_DIR IMAGE LIMINE_CONF SIZE_MB DOOM_WAD DOOM_LICENSE
set -eu

# sgdisk and friends live in the sbin directories, which are not always on a
# non-root user's PATH.
PATH="$PATH:/usr/sbin:/sbin"
export PATH

BUILD_DIR="${BUILD_DIR:-build}"
KERNEL="${KERNEL:-$BUILD_DIR/kernel}"
LIMINE_DIR="${LIMINE_DIR:-$BUILD_DIR/deps/limine-binary}"
IMAGE="${IMAGE:-$BUILD_DIR/helix.img}"
LIMINE_CONF="${LIMINE_CONF:-limine.conf}"
DOOM_WAD="${DOOM_WAD:-}"
DOOM_LICENSE="${DOOM_LICENSE:-}"
SIZE_MB="${SIZE_MB:-64}"

# Geometry chosen so one "cylinder" is exactly 1 MiB, matching the layout
# used by the Limine project's own templates.
HEADS=64
SECTORS_PER_TRACK=32
CYLINDER_SECTORS=$((HEADS * SECTORS_PER_TRACK))
PART_START=$CYLINDER_SECTORS
PART_SECTORS=$(((SIZE_MB - 2) * CYLINDER_SECTORS))
PART_END=$((PART_START + PART_SECTORS - 1))
PART_OFFSET=$((PART_START * 512))

for tool in sgdisk mformat mmd mcopy; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "mkimage: required tool '$tool' not found (install gdisk + mtools)" >&2
        exit 1
    }
done

[ -f "$KERNEL" ] || { echo "mkimage: kernel not found: $KERNEL" >&2; exit 1; }
[ -f "$LIMINE_DIR/BOOTX64.EFI" ] || { echo "mkimage: BOOTX64.EFI not found in $LIMINE_DIR" >&2; exit 1; }
[ -f "$LIMINE_CONF" ] || { echo "mkimage: limine.conf not found: $LIMINE_CONF" >&2; exit 1; }

mkdir -p "$(dirname "$IMAGE")"

echo "[mkimage] creating ${SIZE_MB} MiB image: $IMAGE"
# bs is given in bytes and stderr is discarded so this works with both GNU dd
# (Linux) and BSD dd (macOS), which have different option sets.
dd if=/dev/zero of="$IMAGE" bs=1048576 count="$SIZE_MB" 2>/dev/null

echo "[mkimage] writing GPT + EFI System Partition"
sgdisk "$IMAGE" -n "1:${PART_START}:${PART_END}" -t 1:ef00 >/dev/null

echo "[mkimage] formatting FAT32 filesystem on the ESP"
# -F forces FAT32, which the UEFI specification recommends for the ESP.
mformat -i "$IMAGE@@${PART_OFFSET}" -T "$PART_SECTORS" -h "$HEADS" -s "$SECTORS_PER_TRACK" -F ::
mmd -i "$IMAGE@@${PART_OFFSET}" ::/EFI ::/EFI/BOOT ::/boot ::/boot/limine
if [ -n "$DOOM_WAD" ]; then
    [ -f "$DOOM_WAD" ] || { echo "mkimage: Doom WAD not found: $DOOM_WAD" >&2; exit 1; }
    mmd -i "$IMAGE@@${PART_OFFSET}" ::/boot/doom
fi

echo "[mkimage] installing Limine, config and kernel"
mcopy -i "$IMAGE@@${PART_OFFSET}" "$LIMINE_DIR/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$IMAGE@@${PART_OFFSET}" "$LIMINE_CONF"           ::/boot/limine/limine.conf
mcopy -i "$IMAGE@@${PART_OFFSET}" "$KERNEL"                ::/boot/kernel
if [ -n "$DOOM_WAD" ]; then
    # Limine config is generated in the build directory so the source config
    # stays independent of a user's locally supplied, licensed game data.
    DOOM_CONF="$BUILD_DIR/limine-doom.conf"
    cp "$LIMINE_CONF" "$DOOM_CONF"
    printf '    module_path: boot():/boot/doom/doom1.wad\n    module_string: helix-doom-wad\n' >> "$DOOM_CONF"
    mcopy -i "$IMAGE@@${PART_OFFSET}" "$DOOM_WAD" ::/boot/doom/doom1.wad
    if [ -n "$DOOM_LICENSE" ]; then
        [ -f "$DOOM_LICENSE" ] || { echo "mkimage: Doom asset license not found: $DOOM_LICENSE" >&2; exit 1; }
        mcopy -i "$IMAGE@@${PART_OFFSET}" "$DOOM_LICENSE" ::/boot/doom/COPYING.adoc
    fi
    mcopy -o -i "$IMAGE@@${PART_OFFSET}" "$DOOM_CONF" ::/boot/limine/limine.conf
fi

echo "[mkimage] done: $IMAGE"
