#!/bin/sh
# mkimage.sh — assemble a bootable UEFI disk image.
#
# Layout produced (a 64 MiB GPT disk by default):
#
#   sector 2048 .......................... EFI System Partition (FAT32)
#   sector 65536 ......................... HFS data partition
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
# Environment overrides: BUILD_DIR KERNEL LIMINE_DIR IMAGE LIMINE_CONF SIZE_MB DOOM_WAD DOOM_LICENSE HELIX_APP HELIX_SHELL
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
HELIX_APP="${HELIX_APP:-}"
HELIX_SHELL="${HELIX_SHELL:-}"
SIZE_MB="${SIZE_MB:-64}"

# Keep the ESP compact so the installed disk also has a native HFS partition.
# Sector 2048 is the 1 MiB alignment point used by Limine's templates.
HEADS=64
SECTORS_PER_TRACK=32
CYLINDER_SECTORS=$((HEADS * SECTORS_PER_TRACK))
PART_START=$CYLINDER_SECTORS
ESP_MB=32
PART_SECTORS=$((ESP_MB * CYLINDER_SECTORS - PART_START))
PART_END=$((PART_START + PART_SECTORS - 1))
HFS_PART_START=$((PART_END + 1))
HFS_PART_SECTORS=$((SIZE_MB * CYLINDER_SECTORS - 33 - HFS_PART_START))
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
sgdisk "$IMAGE" -n "1:${PART_START}:${PART_END}" -t 1:ef00 \
    -n "2:${HFS_PART_START}:0" -t 2:8300 >/dev/null

echo "[mkimage] initializing HFS v1 data partition"
python3 "$(dirname "$0")/mkhfs.py" "$IMAGE" \
    --start-sector "$HFS_PART_START" --volume-sectors "$HFS_PART_SECTORS"

echo "[mkimage] formatting the ESP (FAT type chosen from its size)"
# Do NOT force -F (FAT32) here. A FAT32 volume needs at least 65525
# clusters; the default 31.5 MiB ESP has ~63k, and EDK2 classifies a volume
# by cluster count, so it rejects such a "FAT32" and the installed disk
# fails with "Boot0001 ... Not Found". Without -F, mformat picks FAT16 for
# this size (valid for an ESP per the UEFI spec) and FAT32 for larger ones.
mformat -i "$IMAGE@@${PART_OFFSET}" -T "$PART_SECTORS" -h "$HEADS" -s "$SECTORS_PER_TRACK" ::
mmd -i "$IMAGE@@${PART_OFFSET}" ::/EFI ::/EFI/BOOT ::/boot ::/boot/limine
if [ -n "$DOOM_WAD" ]; then
    [ -f "$DOOM_WAD" ] || { echo "mkimage: Doom WAD not found: $DOOM_WAD" >&2; exit 1; }
    mmd -i "$IMAGE@@${PART_OFFSET}" ::/boot/doom
fi
if [ -n "$HELIX_APP" ]; then
    [ -f "$HELIX_APP" ] || { echo "mkimage: Helix app not found: $HELIX_APP" >&2; exit 1; }
    mmd -i "$IMAGE@@${PART_OFFSET}" ::/boot/apps
fi
if [ -n "$HELIX_SHELL" ]; then
    [ -f "$HELIX_SHELL" ] || { echo "mkimage: Helix shell app not found: $HELIX_SHELL" >&2; exit 1; }
    if [ -z "$HELIX_APP" ]; then mmd -i "$IMAGE@@${PART_OFFSET}" ::/boot/apps; fi
fi

echo "[mkimage] installing Limine, config and kernel"
mcopy -i "$IMAGE@@${PART_OFFSET}" "$LIMINE_DIR/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$IMAGE@@${PART_OFFSET}" "$KERNEL"                ::/boot/kernel
CONF_TO_INSTALL="$LIMINE_CONF"
if [ -n "$DOOM_WAD" ] || [ -n "$HELIX_APP" ] || [ -n "$HELIX_SHELL" ]; then
    # Generated config keeps the source config independent of local modules.
    GENERATED_CONF="$BUILD_DIR/limine-modules.conf"
    cp "$LIMINE_CONF" "$GENERATED_CONF"
    if [ -n "$HELIX_APP" ]; then
        printf '    module_path: boot():/boot/apps/hello.hlx\n    module_string: helix-native-app\n' >> "$GENERATED_CONF"
    fi
    if [ -n "$HELIX_SHELL" ]; then
        printf '    module_path: boot():/boot/apps/shell.hlx\n    module_string: helix-user-shell\n' >> "$GENERATED_CONF"
    fi
    if [ -n "$DOOM_WAD" ]; then
        printf '    module_path: boot():/boot/doom/doom1.wad\n    module_string: helix-doom-wad\n' >> "$GENERATED_CONF"
    fi
    CONF_TO_INSTALL="$GENERATED_CONF"
fi
mcopy -i "$IMAGE@@${PART_OFFSET}" "$CONF_TO_INSTALL" ::/boot/limine/limine.conf
if [ -n "$HELIX_APP" ]; then
    mcopy -i "$IMAGE@@${PART_OFFSET}" "$HELIX_APP" ::/boot/apps/hello.hlx
fi
if [ -n "$HELIX_SHELL" ]; then
    mcopy -i "$IMAGE@@${PART_OFFSET}" "$HELIX_SHELL" ::/boot/apps/shell.hlx
fi
if [ -n "$DOOM_WAD" ]; then
    mcopy -i "$IMAGE@@${PART_OFFSET}" "$DOOM_WAD" ::/boot/doom/doom1.wad
    if [ -n "$DOOM_LICENSE" ]; then
        [ -f "$DOOM_LICENSE" ] || { echo "mkimage: Doom asset license not found: $DOOM_LICENSE" >&2; exit 1; }
        mcopy -i "$IMAGE@@${PART_OFFSET}" "$DOOM_LICENSE" ::/boot/doom/COPYING.adoc
    fi
fi

echo "[mkimage] done: $IMAGE"
