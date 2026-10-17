#!/bin/sh
# run-qemu.sh — boot a Helix disk image in QEMU with UEFI firmware.
#
#   sh tools/run-qemu.sh build/helix.img
#
# The kernel prints to serial, which is attached to your terminal. If you
# want to see the graphical framebuffer console instead, run with
#   HEADLESS=0 sh tools/run-qemu.sh build/helix.img
# and a QEMU window will open (requires a desktop session).
set -eu

IMAGE="${1:-build/helix.img}"
OVMF_CODE="${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE.fd}"
OVMF_VARS_SRC="${OVMF_VARS_SRC:-/usr/share/OVMF/OVMF_VARS.fd}"
HEADLESS="${HEADLESS:-1}"
QEMU_MEM="${QEMU_MEM:-512}"

[ -f "$IMAGE" ] || { echo "run-qemu: no image at $IMAGE (run 'make image' first)" >&2; exit 1; }
[ -f "$OVMF_CODE" ] || { echo "run-qemu: OVMF firmware not found at $OVMF_CODE" >&2; exit 1; }

# UEFI variable store must be writable, so work on a throwaway copy.
VARS="$(mktemp /tmp/helix-ovmf-vars.XXXXXX.fd)"
cp "$OVMF_VARS_SRC" "$VARS"
trap 'rm -f "$VARS"' EXIT INT TERM

if [ "$HEADLESS" = "1" ]; then
    DISPLAY_ARGS="-display none"
else
    DISPLAY_ARGS=""
fi

echo "[run-qemu] booting $IMAGE (Ctrl-A X to quit QEMU)"
# shellcheck disable=SC2086
qemu-system-x86_64 \
    -M q35 -m "$QEMU_MEM" \
    -drive if=pflash,format=raw,unit=0,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,unit=1,file="$VARS" \
    -drive file="$IMAGE",format=raw,if=ide,index=0,media=disk \
    -serial stdio $DISPLAY_ARGS
