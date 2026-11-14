#!/bin/sh
# fat32-test.sh — build FAT32 images with mkfs.fat and mtools, run the host self-check.
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
OUT="$BUILD_DIR/fat32test"
SRC="$OUT/src"
rm -rf "$OUT"
mkdir -p "$SRC"

for tool in mkfs.fat mcopy mmd mdel; do
    PATH="$PATH:/usr/sbin:/sbin" command -v "$tool" >/dev/null 2>&1 || {
        echo "[fat32-test] SKIP: $tool not found (install dosfstools and mtools)"; exit 0; }
done
PATH="$PATH:/usr/sbin:/sbin"
export MTOOLS_SKIP_CHECK=1 LANG=C.UTF-8 LC_ALL=C.UTF-8

# --- host copies of the files planted in the images ---
printf 'Hello from FAT32!\n' > "$SRC/hello.txt"          # 18 bytes
printf 'long name works\n'  > "$SRC/readme-long-name.txt" # 16 bytes
printf 'nested file\n'      > "$SRC/nested.txt"           # 12 bytes
printf 'deep file\n'        > "$SRC/deep.txt"             # 10 bytes
printf 'gone\n'             > "$SRC/gone.txt"
printf 'lower\n'            > "$SRC/lower.txt"
printf 'unicode name\n'     > "$SRC/unicode.txt"
# big.bin: 100000 bytes of a pattern that differs at every cluster boundary
awk 'BEGIN { for (i = 0; i < 100000; i++) printf "%c", (i * 7 + int(i / 512) * 13 + 5) % 251 + 1 }' \
    | LC_ALL=C tr -d '\n' > "$SRC/big.bin" 2>/dev/null || true
python3 - "$SRC/big.bin" <<'PY'
import sys
open(sys.argv[1], "wb").write(bytes(((i * 7 + (i // 512) * 13 + 5) % 251) for i in range(100000)))
PY
python3 - "$SRC/frag.bin" <<'PY'
import sys
open(sys.argv[1], "wb").write(bytes(((i * 11 + (i // 512) * 3 + 1) % 253) for i in range(60000)))
PY

populate() {
    img="$1"
    mcopy -i "$img" "$SRC/hello.txt" ::/HELLO.TXT
    mcopy -i "$img" "$SRC/readme-long-name.txt" ::/readme-long-name.txt
    mcopy -i "$img" "$SRC/lower.txt" ::/lower.txt
    mcopy -i "$img" "$SRC/gone.txt" ::/GONE.TXT
    mmd -i "$img" ::/DOCS ::/DOCS/SUB
    mcopy -i "$img" "$SRC/nested.txt" ::/DOCS/NESTED.TXT
    mcopy -i "$img" "$SRC/deep.txt" ::/DOCS/SUB/DEEP.TXT
    mcopy -i "$img" "$SRC/big.bin" ::/BIG.BIN
    mdel -i "$img" ::/GONE.TXT
    # Fragment FRAG.BIN: fill with small files, free every other one, then write the big file.
    mmd -i "$img" ::/TMP
    i=0
    while [ $i -lt 40 ]; do
        printf 'x%d\n' "$i" > "$OUT/tmpfile"
        mcopy -i "$img" "$OUT/tmpfile" "::/TMP/F$i.TXT"
        i=$((i + 1))
    done
    i=0
    while [ $i -lt 40 ]; do
        mdel -i "$img" "::/TMP/F$i.TXT"
        i=$((i + 2))
    done
    mcopy -i "$img" "$SRC/frag.bin" ::/FRAG.BIN
    if mcopy -i "$img" "$SRC/unicode.txt" "::/über-lång-filnamn-ünï.txt" 2>/dev/null &&
       mcopy -i "$img" "$SRC/unicode.txt" "::/emoji-ab-file.txt" 2>/dev/null; then
        : > "$SRC/have-unicode"
        # mtools cannot write characters outside the BMP, so turn the "ab" in the name into
        # the UTF-16 surrogate pair for U+1F600 directly in the long-name entry.
        python3 - "$img" <<'PY'
import sys
p = sys.argv[1]
d = bytearray(open(p, "rb").read())
pat = b"-\0a\0b\0-\0"
i = d.find(pat)
assert i >= 0 and d.find(pat, i + 1) < 0
d[i + 2:i + 6] = bytes([0x3D, 0xD8, 0x00, 0xDE])
open(p, "wb").write(d)
PY
    fi
}

# Image A: small clusters (512 bytes). Image B: 4 KiB clusters, sparse.
truncate -s 34M "$OUT/a.img"
mkfs.fat -F 32 -s 1 -n HELIXFAT "$OUT/a.img" >"$OUT/mkfs-a.log" 2>&1 || { echo "[fat32-test] FAIL: mkfs A"; cat "$OUT/mkfs-a.log"; exit 1; }
populate "$OUT/a.img"
truncate -s 300M "$OUT/b.img"
mkfs.fat -F 32 -s 8 -n HELIXFAT "$OUT/b.img" >"$OUT/mkfs-b.log" 2>&1 || { echo "[fat32-test] FAIL: mkfs B"; cat "$OUT/mkfs-b.log"; exit 1; }
populate "$OUT/b.img"
truncate -s 20M "$OUT/c.img"
mkfs.fat -F 16 "$OUT/c.img" >"$OUT/mkfs-c.log" 2>&1 || { echo "[fat32-test] FAIL: mkfs FAT16"; exit 1; }

cc -std=gnu11 -Wall -Wextra -o "$OUT/fat32-test" "$ROOT/tests/fat32-test.c" \
    "$ROOT/kernel/src/fat32.c" "$ROOT/kernel/src/block_device.c" \
    -I"$ROOT/kernel/src" -I"$ROOT/kernel/include"

cd "$OUT"
./fat32-test "$SRC" "$OUT/a.img" 1 "$OUT/b.img" 8 "$OUT/c.img"
