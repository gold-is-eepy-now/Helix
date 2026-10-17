#!/bin/sh
# mkdisk.sh — convert the raw disk image into hypervisor disk formats.
#
# A raw .img works with QEMU, but tools like VirtualBox, VMware and Hyper-V
# want their own container format. qemu-img converts between them.
#
#   sh tools/mkdisk.sh build/helix.img build
#
# Produces helix.vdi (VirtualBox), helix.vmdk (VMware/VirtualBox) and
# helix.vhd (Hyper-V/Virtual PC).
set -eu

IMAGE="${1:-build/helix.img}"
OUTDIR="${2:-$(dirname "$IMAGE")}"

[ -f "$IMAGE" ] || { echo "mkdisk: no image at $IMAGE (run 'make image' first)" >&2; exit 1; }
command -v qemu-img >/dev/null 2>&1 || {
    echo "mkdisk: qemu-img not found (Debian/Ubuntu: 'sudo apt install qemu-utils'; macOS: 'brew install qemu')" >&2
    exit 1
}

mkdir -p "$OUTDIR"
for fmt in vdi vmdk vpc; do
    case "$fmt" in
        vdi) ext="vdi" ;;
        vmdk) ext="vmdk" ;;
        vpc) ext="vhd" ;;
    esac
    out="$OUTDIR/helix.$ext"
    echo "[mkdisk] $IMAGE -> $out ($fmt)"
    qemu-img convert -f raw -O "$fmt" "$IMAGE" "$out"
done

echo "[mkdisk] done: $OUTDIR/helix.vdi $OUTDIR/helix.vmdk $OUTDIR/helix.vhd"
