#!/usr/bin/env bash
# Font payload matrix: what each embedded face costs the default wasm fetch.
#
# WHY: the case for externalizing a face is a byte argument, and the byte argument flips when the
# default face changes. This measures all four combinations with the same compiler and flags so the
# numbers are comparable, and reports Brotli (the transport encoding, not the raw module size).
#
# usage: font_payload_matrix.sh [--jobs N]
set -uo pipefail
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"
JOBS="${JOBS:-8}"
OUT="${OUT:-/tmp/wasm-matrix}"
mkdir -p "$OUT"

for cfg in kopub+ridi kopub-only ridi-only neither; do
  case "$cfg" in
    kopub+ridi) ridi=ON;  kopub=ON  ;;
    kopub-only) ridi=OFF; kopub=ON  ;;
    ridi-only)  ridi=ON;  kopub=OFF ;;
    neither)    ridi=OFF; kopub=OFF ;;
  esac
  dir="$OUT/build-$cfg"
  if [ ! -f "$dir/ko_xtch_wasm.wasm" ]; then
    echo "== building $cfg (KO_EMBED_RIDI=$ridi KO_EMBED_KOPUB=$kopub) =="
    emcmake cmake -S . -B "$dir" -DCMAKE_BUILD_TYPE=Release \
      -DKO_EMBED_RIDI=$ridi -DKO_EMBED_KOPUB=$kopub >"$OUT/$cfg.cmake.log" 2>&1 || {
        echo "  cmake FAILED (see $OUT/$cfg.cmake.log)"; continue; }
    cmake --build "$dir" -j"$JOBS" --target ko_xtch_wasm >"$OUT/$cfg.build.log" 2>&1 || {
        echo "  build FAILED (see $OUT/$cfg.build.log)"; continue; }
  fi
  raw=$(stat -f%z "$dir/ko_xtch_wasm.wasm" 2>/dev/null || echo 0)
  br=$(brotli -q 11 -c "$dir/ko_xtch_wasm.wasm" 2>/dev/null | wc -c | tr -d ' ')
  printf '%-12s raw %9s  brotli %9s\n' "$cfg" "$raw" "$br" | tee -a "$OUT/matrix.txt"
done
echo
echo "== summary (Brotli, the number that matters for the fetch) =="
cat "$OUT/matrix.txt"
