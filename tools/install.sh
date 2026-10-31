#!/bin/sh
# install.sh — create and populate the ext4 filesystem image.
#
# This script is intended to be run as part of `make install`. It creates an
# ext4 image with the directory structure needed for the Helix OS root
# filesystem, including /etc, /var, and /home directories.

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
OUT="$BUILD_DIR/ext4-root.img"

echo "[install] creating ext4 filesystem image: $OUT"

# Create 8 MiB ext4 image
mkdir -p "$(dirname "$OUT")"
dd if=/dev/zero of="$OUT" bs=1M count=8 status=none >/dev/null 2>&1

# Format with default ext4 settings (extents, 64-bit group descriptors,
# flex_bg, extra_isize) — matches what the ext4 driver supports.
mke2fs -F -q -t ext4 -L helix-root "$OUT" >"${OUT}.mkfs.log" 2>&1

# Create temporary files for population
TMPDIR="$BUILD_DIR/install-tmp"
mkdir -p "$TMPDIR"

# /etc/hostname
echo "helix-test-host" > "$TMPDIR/hostname"

# /etc/os-release
cat > "$TMPDIR/os-release" <<EOF
NAME="Helix OS"
PRETTY_NAME="Helix OS"
VERSION_ID="1.0"
HOME_URL="https://github.com/helix-os/Helix"
EOF

# /var/big.txt — 3072 lines x 48 bytes = 144 KiB, matching tests/ext4-test.c
: > "$TMPDIR/big.txt"
for i in $(seq 0 3071); do
    printf 'line %05d: helix ext4 payload 0123456789abcdef\n' "$i" >> "$TMPDIR/big.txt"
done

# Populate the image via debugfs
debugfs -w -f <(cat <<EOF
mkdir /etc
mkdir /home
mkdir /var
write $TMPDIR/hostname /etc/hostname
write $TMPDIR/os-release /etc/os-release
write $TMPDIR/big.txt /var/big.txt
EOF
) "$OUT" >"${OUT}.debugfs.log" 2>&1

# Verify the image
echo "[install] testing the ext4 image..."
debugfs -R 'ls -l /' "$OUT" 2>&1 | head -20
echo ""
debugfs -R 'stat /etc/hostname' "$OUT" 2>&1 | head -5
echo ""
debugfs -R 'stat /var/big.txt' "$OUT" 2>&1 | head -5

# Cleanup
rm -rf "$TMPDIR"

echo "[install] ext4 image created and populated at $OUT"
echo "  Size: $(du -h "$OUT" | cut -f1)"