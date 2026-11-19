#!/bin/sh
# install-dependencies-apt.sh — install Helix's build/test dependencies on
# Debian/Ubuntu with apt-get. Run from anywhere:  ./install-dependencies-apt.sh
#
# Covers what README.md's Requirements section lists, plus what the tools and
# tests shell out to (python3, dosfstools, e2fsprogs, xorriso, qemu-utils).
# Note: the x86_64-elf cross compiler is NOT an apt package; the host gcc
# builds Helix fine (make), install x86_64-elf-gcc yourself only if you want
# to run the CLAUDE.md sequence with CC=x86_64-elf-gcc.
set -eu

if [ "$(id -u)" -eq 0 ]; then
    SUDO=""
else
    SUDO="sudo"
fi

$SUDO apt-get update

$SUDO apt-get install -y \
    build-essential \
    curl \
    git \
    python3 \
    qemu-system-x86 \
    qemu-utils \
    ovmf \
    gdisk \
    mtools \
    dosfstools \
    e2fsprogs \
    xorriso

# Verify: every command the build, tools and tests invoke must now exist.
missing=""
for t in gcc make curl git python3 qemu-system-x86_64 qemu-img \
         sgdisk mformat mcopy mkfs.fat mke2fs xorriso; do
    command -v "$t" >/dev/null 2>&1 || missing="$missing $t"
done

if [ -n "$missing" ]; then
    echo "install-dependencies-apt: MISSING after install:$missing" >&2
    exit 1
fi

# OVMF firmware is files, not a command; tools/ovmf.sh looks in several paths.
if [ ! -d /usr/share/OVMF ] && [ ! -d /usr/share/ovmf ] \
   && [ ! -d /usr/share/qemu ]; then
    echo "install-dependencies-apt: WARNING: no OVMF directory found;" \
         "make run may fail (tools/ovmf.sh checks the usual paths)" >&2
fi

echo "install-dependencies-apt: all dependencies present."
