# ovmf.sh — locate UEFI (OVMF/EDK2) firmware for QEMU.
#
# Source this file; it sets OVMF_CODE and OVMF_VARS_SRC unless they are already
# set in the environment. If you set them yourself, they win.
#
#   . "$(dirname "$0")/ovmf.sh"
#
# We probe the common locations on Linux and macOS. OVMF ships as a "code"
# image (read-only firmware) plus a "vars" template (a writable NVRAM store
# that QEMU copies and mutates).
#
# Note: on Homebrew's qemu the vars file is named edk2-i386-vars.fd even for an
# x86_64 guest; it is the architecture-independent variable store.

OVMF_CODE="${OVMF_CODE:-}"
OVMF_VARS_SRC="${OVMF_VARS_SRC:-}"

_ovmf_try() {
    if [ -f "$1" ] && [ -f "$2" ]; then
        OVMF_CODE="$1"
        OVMF_VARS_SRC="$2"
        return 0
    fi
    return 1
}

if [ -z "$OVMF_CODE" ] || [ -z "$OVMF_VARS_SRC" ]; then
    _ovmf_try /usr/share/OVMF/OVMF_CODE.fd \
              /usr/share/OVMF/OVMF_VARS.fd \
    || _ovmf_try /opt/homebrew/share/qemu/edk2-x86_64-code.fd \
                 /opt/homebrew/share/qemu/edk2-i386-vars.fd \
    || _ovmf_try /usr/local/share/qemu/edk2-x86_64-code.fd \
                 /usr/local/share/qemu/edk2-i386-vars.fd \
    || _ovmf_try /usr/share/qemu/edk2-x86_64-code.fd \
                 /usr/share/qemu/edk2-i386-vars.fd \
    || _ovmf_try /usr/share/edk2/x86_64/OVMF_CODE.fd \
                 /usr/share/edk2/x86_64/OVMF_VARS.fd \
    || true
fi
