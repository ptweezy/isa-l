#!/bin/bash
# Instruction-level profile of one decoder in one column.
#
#   tools/profile.sh BINARY BACKEND FILE [COLUMN] [ROUNDS]
#
# Runs BINARY on FILE with only BACKEND and only COLUMN (default "raw, one
# call"), with the benchmark's own sampler on (bench --prof), and prints the
# hottest instructions with the disassembly around them.
#
#   tools/profile.sh ./bench-zlib-v8 "zlib @" corpus/silesia.cat
#   tools/profile.sh ./bench-zlib-v8 "zlib @" corpus/silesia.cat "16 KiB stream"
set -euo pipefail

BIN=$1
BACKEND=$2
FILE=$3
COL=${4:-raw, one call}
ROUNDS=${5:-60}
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-profile-out}
mkdir -p "$OUT"
clean() { echo "$1" | tr -c 'A-Za-z0-9\n' '_'; }
TAG=$(basename "$BIN")-$(clean "$BACKEND")-$(clean "$COL")-$(basename "$FILE")

"$BIN" --max-mb 256 --rounds "$ROUNDS" --only "$BACKEND" --cols "$COL" \
	--prof "$OUT/$TAG.pcs" "$FILE" | grep -E "^\| " | tail -1
python3 "$HERE/hot.py" "$OUT/$TAG.pcs" "$BIN" | tee "$OUT/$TAG.hot.txt"
