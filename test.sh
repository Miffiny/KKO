#!/usr/bin/env bash
set -euo pipefail

BIN="./lz_codec"
RAW="test.raw"
CMP="test.lz"
OUT="test_out.raw"

WIDTH=256
HEIGHT=256

echo "[1/5] Generating compressible RAW image: ${WIDTH}x${HEIGHT}"

python3 - <<'PY'
width = 256
height = 256

with open("test.raw", "wb") as f:
    for y in range(height):
        # 32-row horizontal bands with constant grayscale value
        value = ((y // 32) * 32) % 256
        row = bytes([value]) * width
        f.write(row)
PY

echo "[2/5] Original size:"
stat -c "%n %s bytes" "$RAW"

echo "[3/5] Compressing"
"$BIN" -c -i "$RAW" -o "$CMP" -w "$WIDTH" -a -m

echo "[4/5] Compressed size:"
stat -c "%n %s bytes" "$CMP"

echo "[5/5] Decompressing and verifying"
"$BIN" -d -i "$CMP" -o "$OUT"

cmp -s "$RAW" "$OUT" && echo "Round-trip OK" || { echo "Round-trip FAILED"; exit 1; }

orig_size=$(stat -c "%s" "$RAW")
cmp_size=$(stat -c "%s" "$CMP")

if [ "$cmp_size" -lt "$orig_size" ]; then
    echo "Compression successful: $orig_size -> $cmp_size bytes"
else
    echo "No gain from compression: $orig_size -> $cmp_size bytes"
fi