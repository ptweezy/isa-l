#!/bin/bash
# Instruction-level profile of one decoder on macOS.
#
#   tools/profile.sh BINARY BACKEND FILE [COLUMN] [SECONDS]
#
# Runs BINARY on FILE with only BACKEND and only COLUMN (default "raw, one
# call"), samples it once a millisecond with sample(1), and prints the hottest
# instructions of every function that took at least 2% of the samples, with the
# disassembly around them.
#
#   tools/profile.sh ./bench-zlib-v5h "zlib @" corpus/silesia.cat
#   tools/profile.sh ./bench "system zlib" corpus/silesia.cat
set -euo pipefail

BIN=$1
BACKEND=$2
FILE=$3
COL=${4:-raw, one call}
SECS=${5:-15}
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-profile-out}
mkdir -p "$OUT"
TAG=$(basename "$BIN")-$(echo "$BACKEND" | tr -c 'A-Za-z0-9\n' '_')-$(basename "$FILE")

# Enough rounds to outlast the sampling; the run is killed afterwards.
"$BIN" --max-mb 256 --rounds 100000 --only "$BACKEND" --cols "$COL" "$FILE" \
	> /dev/null 2>&1 &
PID=$!
trap 'kill $PID 2>/dev/null || true' EXIT

# The reference compression comes first (about 10 s for 256 MiB at level 6).
sleep "${WARM:-20}"
sample "$PID" "$SECS" 1 -file "$OUT/$TAG.sample" > /dev/null 2>&1
kill "$PID" 2>/dev/null || true
trap - EXIT

python3 "$HERE/hot.py" "$OUT/$TAG.sample" "$BIN" | tee "$OUT/$TAG.hot.txt"
