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
# Optional USB host pass-through:
#   USB_HOST_VID=0x1234 USB_HOST_PID=0xabcd make run
# or select the current host USB bus and address:
#   USB_HOST_BUS=1 USB_HOST_ADDR=4 make run
# Attach a disposable raw USB disk image instead of a physical host device:
#   USB_IMAGE=build/hfs-test.img make run
# Select EHCI for USB 2.0 emulation (default is xHCI):
#   USB_CONTROLLER=ehci USB_IMAGE=build/ehci-usb-test.img make run
set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
. "$SCRIPT_DIR/ovmf.sh"

IMAGE="${1:-build/helix.img}"
HEADLESS="${HEADLESS:-1}"
QEMU_MEM="${QEMU_MEM:-512}"
QEMU_ACCEL="${QEMU_ACCEL:-}"
USB_HOST_VID="${USB_HOST_VID:-}"
USB_HOST_PID="${USB_HOST_PID:-}"
USB_HOST_BUS="${USB_HOST_BUS:-}"
USB_HOST_ADDR="${USB_HOST_ADDR:-}"
USB_IMAGE="${USB_IMAGE:-}"
USB_IMAGE_FORMAT="${USB_IMAGE_FORMAT:-raw}"
USB_CONTROLLER="${USB_CONTROLLER:-xhci}"

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

ACCEL_ARGS=""
if [ -n "$QEMU_ACCEL" ]; then ACCEL_ARGS="-accel $QEMU_ACCEL"; fi

case "$USB_CONTROLLER" in
    xhci) USB_ARGS="-device qemu-xhci,id=helix-xhci" ;;
    ehci) USB_ARGS="-device usb-ehci,id=helix-ehci" ;;
    *) echo "run-qemu: USB_CONTROLLER must be xhci or ehci" >&2; exit 1 ;;
esac
USB_IMAGE_ARGS=""
if [ -n "$USB_IMAGE" ]; then
    [ -f "$USB_IMAGE" ] || { echo "run-qemu: USB image not found: $USB_IMAGE" >&2; exit 1; }
    case "$USB_IMAGE_FORMAT" in raw|qcow2) ;; *)
        echo "run-qemu: USB_IMAGE_FORMAT must be raw or qcow2" >&2; exit 1;;
    esac
    [ -z "$USB_HOST_VID$USB_HOST_PID$USB_HOST_BUS$USB_HOST_ADDR" ] || {
        echo "run-qemu: choose USB_IMAGE or USB host pass-through, not both" >&2; exit 1;
    }
    USB_IMAGE_ARGS="-drive if=none,id=helix-usb-drive,file=$USB_IMAGE,format=$USB_IMAGE_FORMAT -device usb-storage,drive=helix-usb-drive"
fi
if [ -n "$USB_HOST_VID" ] || [ -n "$USB_HOST_PID" ]; then
    [ -n "$USB_HOST_VID" ] && [ -n "$USB_HOST_PID" ] || {
        echo "run-qemu: set both USB_HOST_VID and USB_HOST_PID" >&2; exit 1;
    }
    [ -z "$USB_HOST_BUS" ] && [ -z "$USB_HOST_ADDR" ] || {
        echo "run-qemu: choose VID/PID or bus/address, not both" >&2; exit 1;
    }
    for id in "$USB_HOST_VID" "$USB_HOST_PID"; do
        hex=${id#0x}; hex=${hex#0X}
        [ -n "$hex" ] || { echo "run-qemu: invalid USB vendor/product ID" >&2; exit 1; }
        case "$hex" in *[!0123456789aAbBcCdDeEfF]*)
            echo "run-qemu: USB vendor/product IDs must be hexadecimal" >&2; exit 1;;
        esac
    done
    USB_ARGS="$USB_ARGS -device usb-host,guest-reset=false,vendorid=$USB_HOST_VID,productid=$USB_HOST_PID"
elif [ -n "$USB_HOST_BUS" ] || [ -n "$USB_HOST_ADDR" ]; then
    [ -n "$USB_HOST_BUS" ] && [ -n "$USB_HOST_ADDR" ] || {
        echo "run-qemu: set both USB_HOST_BUS and USB_HOST_ADDR" >&2; exit 1;
    }
    for number in "$USB_HOST_BUS" "$USB_HOST_ADDR"; do
        case "$number" in ''|*[!0-9]*)
            echo "run-qemu: USB bus and address must be decimal numbers" >&2; exit 1;;
        esac
    done
    USB_ARGS="$USB_ARGS -device usb-host,guest-reset=false,hostbus=$USB_HOST_BUS,hostaddr=$USB_HOST_ADDR"
fi

echo "[run-qemu] booting $IMAGE (Ctrl-A X to quit QEMU)"
# shellcheck disable=SC2086
qemu-system-x86_64 \
    -M q35 -m "$QEMU_MEM" \
    $ACCEL_ARGS \
    -drive if=pflash,format=raw,unit=0,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,unit=1,file="$VARS" \
    -drive file="$IMAGE",format=raw,if=ide,index=0,media=disk \
    -serial stdio $DISPLAY_ARGS $USB_ARGS $USB_IMAGE_ARGS
