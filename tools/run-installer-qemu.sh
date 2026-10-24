#!/bin/sh
# Boot an installer ISO with a disposable qcow2 target attached as SATA or USB.
set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
. "$SCRIPT_DIR/ovmf.sh"

ISO="${1:-build/helix.iso}"
TARGET="${2:-build/helix-target.qcow2}"
QEMU="${QEMU:-qemu-system-x86_64}"
QEMU_ACCEL="${QEMU_ACCEL:-}"
INSTALL_TARGET_BUS="${INSTALL_TARGET_BUS:-sata}"
USB_CONTROLLER="${USB_CONTROLLER:-xhci}"

[ -f "$ISO" ] || { echo "run-installer-qemu: ISO not found: $ISO" >&2; exit 1; }
[ -f "$TARGET" ] || { echo "run-installer-qemu: target qcow2 not found: $TARGET" >&2; exit 1; }
[ -f "$OVMF_CODE" ] || { echo "run-installer-qemu: set OVMF_CODE to EDK2/OVMF code firmware" >&2; exit 1; }
[ -f "$OVMF_VARS_SRC" ] || { echo "run-installer-qemu: set OVMF_VARS_SRC to EDK2/OVMF variables template" >&2; exit 1; }
command -v "$QEMU" >/dev/null 2>&1 || { echo "run-installer-qemu: QEMU not found" >&2; exit 1; }

VARS="$(mktemp "${TMPDIR:-/tmp}/helix-installer-vars.XXXXXX")"
cp "$OVMF_VARS_SRC" "$VARS"
trap 'rm -f "$VARS"' EXIT INT TERM

ACCEL_ARGS=""
if [ -n "$QEMU_ACCEL" ]; then ACCEL_ARGS="-accel $QEMU_ACCEL"; fi
USB_ARGS=""
case "$USB_CONTROLLER" in
    xhci) USB_ARGS="-device qemu-xhci,id=helix-xhci"; USB_BUS=helix-xhci.0 ;;
    ehci) USB_ARGS="-device usb-ehci,id=helix-ehci"; USB_BUS=helix-ehci.0 ;;
    *) echo "run-installer-qemu: USB_CONTROLLER must be xhci or ehci" >&2; exit 1 ;;
esac

case "$INSTALL_TARGET_BUS" in
    sata)
        TARGET_ARGS="-drive file=$TARGET,format=qcow2,if=ide,index=1,media=disk"
        ;;
    usb)
        TARGET_ARGS="-drive if=none,id=helix-target,file=$TARGET,format=qcow2 -device usb-storage,bus=$USB_BUS,drive=helix-target"
        ;;
    *) echo "run-installer-qemu: INSTALL_TARGET_BUS must be sata or usb" >&2; exit 1 ;;
esac

echo "[run-installer-qemu] booting $ISO; target $TARGET on $INSTALL_TARGET_BUS"
# shellcheck disable=SC2086
"$QEMU" -M q35 -m 512 $ACCEL_ARGS \
    -drive "if=pflash,format=raw,unit=0,readonly=on,file=$OVMF_CODE" \
    -drive "if=pflash,format=raw,unit=1,file=$VARS" \
    -cdrom "$ISO" -boot order=d $USB_ARGS $TARGET_ARGS \
    -serial stdio
