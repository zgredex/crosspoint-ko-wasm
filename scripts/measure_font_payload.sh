#!/usr/bin/env bash
# Measure what the embedded fonts cost the engine wasm payload.
#
#   bash scripts/measure_font_payload.sh
#
# Re-verifies that the default build is untouched by the measurement switches, then builds each
# variant in its own build-perf-<name> directory and prints raw / gzip -9 / brotli -q 11 sizes.
# Results and the decision they led to: docs/ko-font-payload-measurement.md
set -u
cd "$(dirname "$0")/.." || exit 1

if [ ! -f build-wasm/CMakeCache.txt ]; then
  echo "build-wasm/ is not configured; configure it (emcmake) before measuring." >&2
  exit 1
fi
TOOLCHAIN=$(sed -n 's/^CMAKE_TOOLCHAIN_FILE:FILEPATH=//p' build-wasm/CMakeCache.txt | head -1)
if [ -z "$TOOLCHAIN" ]; then
  echo "cannot determine the toolchain file from build-wasm/CMakeCache.txt" >&2
  exit 1
fi
EXPECT_SHA=${EXPECT_SHA:-$(shasum -a 256 build-wasm/ko_xtch_wasm.wasm | awk '{print substr($1,1,16)}')}

measure() {   # $1 = label, $2 = wasm path
  local f="$2" raw gz br
  if [ ! -f "$f" ]; then echo "$(printf '%-14s' "$1") MISSING ($f)"; return; fi
  raw=$(wc -c < "$f" | tr -d ' ')
  gz=$(gzip -9 -c "$f" | wc -c | tr -d ' ')
  br=$(brotli -q 11 -c "$f" | wc -c | tr -d ' ')
  printf '%-14s raw=%9d gzip=%9d brotli=%9d\n' "$1" "$raw" "$gz" "$br"
}

echo "--- default build must be unchanged (the switches are inert unless a variant is requested) ---"
cmake --build build-wasm -j8 >/dev/null 2>&1
BASE_SHA=$(shasum -a 256 build-wasm/ko_xtch_wasm.wasm | awk '{print substr($1,1,16)}')
if [ "$BASE_SHA" = "$EXPECT_SHA" ]; then
  echo "default UNCHANGED ($BASE_SHA)"
else
  echo "default CHANGED: $BASE_SHA (expected $EXPECT_SHA) - the switches are not inert, stop and look" >&2
fi
measure baseline build-wasm/ko_xtch_wasm.wasm

echo "--- variants ---"
build_variant() {
  local label="$1"; shift
  local dir="build-perf-$label"
  cmake -S . -B "$dir" -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" -DCMAKE_BUILD_TYPE=Release "$@" \
    >"/tmp/cfg_$label.log" 2>&1 || { echo "$label CONFIGURE FAILED (/tmp/cfg_$label.log)"; return; }
  cmake --build "$dir" -j8 >"/tmp/build_$label.log" 2>&1 || {
    echo "$label BUILD FAILED:"; tail -3 "/tmp/build_$label.log"; return; }
  measure "$label" "$dir/ko_xtch_wasm.wasm"
}

build_variant no-kopub      -DKO_EMBED_KOPUB=OFF
build_variant no-ridi       -DKO_EMBED_RIDI=OFF
build_variant no-pretendard -DKO_EMBED_PRETENDARD=OFF
build_variant core          -DKO_EMBED_RIDI=OFF -DKO_EMBED_KOPUB=OFF -DKO_EMBED_PRETENDARD=OFF
echo "--- done; the build-perf-* directories and their artifacts are disposable ---"
