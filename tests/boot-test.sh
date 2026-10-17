#!/bin/sh
# boot-test.sh — boot the image headlessly and verify the milestone.
#
# This is the project's most important safety net: it is the machine-checked
# definition of "Milestone 1 works". It boots the image under QEMU + UEFI,
# captures the kernel's serial output, and requires the three banner lines to
# be present. It also grabs a screenshot of the framebuffer console.
#
# Exit status is 0 only if every required line was found.
set -eu

# Locate UEFI firmware (Linux and macOS paths) unless the environment says.
SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
. "$SCRIPT_DIR/../tools/ovmf.sh"

IMAGE="${1:-build/helix.img}"
BUILD_DIR="${BUILD_DIR:-build}"
OUTDIR="${OUTDIR:-$BUILD_DIR/test}"
LOG="$OUTDIR/serial.log"
PPM="$OUTDIR/screen.ppm"
PNG="$OUTDIR/screen.png"
VARS="$OUTDIR/OVMF_VARS.fd"

QEMU_MEM="${QEMU_MEM:-512}"

# `timeout` is GNU coreutils. macOS has it only as `gtimeout` (if coreutils is
# installed), and may not have it at all. QEMU quits on its own via the monitor
# below, so this is only a safety net.
if command -v timeout >/dev/null 2>&1; then
    TIMEOUT="timeout 60"
elif command -v gtimeout >/dev/null 2>&1; then
    TIMEOUT="gtimeout 60"
else
    TIMEOUT=""
fi

mkdir -p "$OUTDIR"
cp "$OVMF_VARS_SRC" "$VARS"

echo "[test] booting $IMAGE headlessly..."
# Feed the QEMU monitor a few commands: wait for boot, screenshot, quit.
# `timeout` is a belt-and-braces guard in case QEMU ignores `quit`.
(
    sleep 15
    echo "screendump $PPM"
    sleep 3
    echo "quit"
) | $TIMEOUT qemu-system-x86_64 \
        -M q35 -m "$QEMU_MEM" \
        -drive if=pflash,format=raw,unit=0,readonly=on,file="$OVMF_CODE" \
        -drive if=pflash,format=raw,unit=1,file="$VARS" \
        -drive file="$IMAGE",format=raw,if=ide,index=0,media=disk \
        -serial "file:$LOG" \
        -display none \
        -monitor stdio \
        -no-reboot >/dev/null 2>&1 || true

echo "[test] --- captured serial output ---"
cat "$LOG" 2>/dev/null || echo "(no serial output captured)"
echo "[test] ------------------------------"

fail=0
for needle in "HELIX OS" "Native Object Kernel" "Boot successful."; do
    if grep -qF "$needle" "$LOG" 2>/dev/null; then
        echo "[test] PASS: found \"$needle\""
    else
        echo "[test] FAIL: missing \"$needle\""
        fail=1
    fi
done

if [ -f "$PPM" ]; then
    if python3 tools/ppm2png.py "$PPM" "$PNG" >/dev/null 2>&1; then
        echo "[test] screenshot: $PNG"
    else
        echo "[test] screenshot: $PPM (PNG conversion failed)"
    fi
else
    echo "[test] note: no framebuffer screenshot was produced"
fi

if [ "$fail" -eq 0 ]; then
    echo "[test] RESULT: PASS"
else
    echo "[test] RESULT: FAIL"
    exit 1
fi
