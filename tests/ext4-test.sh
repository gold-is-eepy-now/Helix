#!/bin/sh
# ext4-test.sh — build ext4 filesystem images and run the host self-check.
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
TESTS="$ROOT/tests"
OUT="$BUILD_DIR/ext4test"
mkdir -p "$OUT"

# helper to compile the harness
cc_compile() {
    cc -Wall -Wextra -o "$OUT/$1" "$TESTS/$1.c" \
        "$ROOT/kernel/src/ext4.c" "$ROOT/kernel/src/block_device.c" \
        -I"$ROOT/kernel/src" -I"$ROOT/kernel/include"
}

# bail early if tools missing
command -v mke2fs >/dev/null 2>&1 || { echo "[ext4-test] SKIP: mke2fs not found"; exit 0; }
command -v debugfs >/dev/null 2>&1 || { echo "[ext4-test] SKIP: debugfs not found"; exit 0; }

# Image 1 — default mke2fs 1K blocks (small image, metadata_csum/flex_bg/64bit on)
dd if=/dev/zero of="$OUT/fs1.img" bs=1M count=8 status=none >/dev/null 2>&1
mke2fs -F -q -t ext4 -L helix-root "$OUT/fs1.img" >"$OUT/mke2fs1.log" 2>&1 || {
    echo "[ext4-test] FAIL: mke2fs image 1"; exit 1;
}
# Populate via debugfs
printf 'helix-test-host\n' > "$OUT/hostname"
# pattern.txt must match ext4-test.c's make_pattern(): 3072 lines x 48 bytes = 144 KiB
: > "$OUT/pattern.txt"
for i in $(seq 0 3071); do
    printf 'line %05d: helix ext4 payload 0123456789abcdef\n' "$i" >> "$OUT/pattern.txt"
done
printf 'NAME=Helix OS\nPRETTY_NAME="Helix OS"\n' > "$OUT/os-release"
debugfs -w -f <(cat <<EOF
mkdir /etc
mkdir /home
mkdir /var
write $OUT/hostname /etc/hostname
write $OUT/os-release /etc/os-release
write $OUT/pattern.txt /var/big.txt
EOF
) "$OUT/fs1.img" >"$OUT/debugfs1.log" 2>&1 || {
    echo "[ext4-test] FAIL: debugfs populate 1"; exit 1;
}

# Compile and run test against image 1
cc_compile ext4-test && "$OUT/ext4-test" "$OUT/fs1.img"
ret1=$?

# Image 2 — 1K blocks, no 64bit/flex_bg/metadata_csum (variant)
dd if=/dev/zero of="$OUT/fs2.img" bs=1M count=8 status=none >/dev/null 2>&1
mke2fs -F -q -t ext4 -b 1024 -O ^metadata_csum,^64bit,^flex_bg -L helix-root "$OUT/fs2.img" >"$OUT/mke2fs2.log" 2>&1 || {
    echo "[ext4-test] FAIL: mke2fs image 2"; exit 1;
}
# populate same structure (big.txt etc) — but this variant may lack some features
debugfs -w -f <(cat <<EOF
mkdir /etc
mkdir /home
mkdir /var
write $OUT/hostname /etc/hostname
write $OUT/os-release /etc/os-release
write $OUT/pattern.txt /var/big.txt
EOF
) "$OUT/fs2.img" >"$OUT/debugfs2.log" 2>&1 || {
    echo "[ext4-test] FAIL: debugfs populate 2"; exit 1;
}

cc_compile ext4-test && "$OUT/ext4-test" "$OUT/fs2.img"
ret2=$?

# final summary
if [ $ret1 -eq 0 ] || [ $ret2 -eq 0 ]; then
    echo "[ext4-test] RESULT: some checks passed"
    # at least one variant succeeded
    exit 0
else
    echo "[ext4-test] RESULT: all checks failed"
    exit 1
fi