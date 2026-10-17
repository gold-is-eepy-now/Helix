#!/bin/sh
# run-qemu.sh — boot a Helix disk image in QEMU with UEFI firmware.
#
#   sh tools/run-qemu.sh build/helix.img
#
# The kernel prints to serial, which is attached to your terminal. If you
# want to see the graphical framebuffer console instead, run with
#   HEADLESS=0 sh tools/run-qemu.sh build/helix.img
# and a QEMU window will open (requires a desktop session).
#
# Firmware is auto-detected (see tools/ovmf.sh); override with the OVMF_CODE
# and OVMF_VARS_SRC environment variables if needed.
set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
. "$SCRIPT_DIR/ovmf.sh"

IMAGE="${1:-build/helix.img}"
HEADLESS="${HEADLESS:-1}"
QEMU_MEM="${QEMU_MEM:-512}"

[ -f "$IMAGE" ] || { echo "run-qemu: no image at $IMAGE (run 'make image' first)" >&2; exit 1; }
if [ -z "$OVMF_CODE" ] || [ ! -f "$OVMF_CODE" ]; then
    echo "run-qemu: UEFI firmware not found." >&2
    echo "  Install it (Debian/Ubuntu: 'sudo apt install ovmf'; macOS: 'brew install qemu')" >&2
    echo "  or set OVMF_CODE and OVMF_VARS_SRC explicitly." >&2
    exit 1
fi
[ -f "$OVMF_VARS_SRC" ] || { echo "run-qemu: OVMF vars not found at $OVMF_VARS_SRC" >&2; exit 1; }

# The UEFI variable store must be writable, so work on a throwaway copy.
# mktemp's template must end in X's (BSD/macOS requirement), so we do not add
# a suffix after them.
VARS="$(mktemp "${TMPDIR:-/tmp}/helix-ovmf-vars.XXXXXX")"
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
