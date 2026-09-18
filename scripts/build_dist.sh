#!/usr/bin/env bash
# Build the deployable static bundle for Cloudflare Pages (or any static host).
#
# WHY THIS EXISTS instead of "just point the host at web/": web/demo.epub is a commercial,
# non-redistributable test book (see .gitignore). It lives on disk for local testing but must never be
# uploaded. Building dist/ with an explicit exclude list makes that structural rather than a thing
# someone has to remember.
#
# THE ONE AUTHORITY: the engine build says which reader faces it carries (ko_build_info.json, written
# by CMake from the same switches that decide what is compiled in). This script derives everything
# else from that:
#
#   web/          ── source only (page, worker, CSS, fixtures) — no engine artifacts
#   build-wasm/   ── the engine (js, wasm, build info)
#   build/        ── the host font tool that generates EPD2 blobs for the faces the engine lacks
#        └──> dist/
#
# and then ASSERTS the result is self-consistent: every face the module does not embed must have a
# blob in dist/, and the manifest must name it. A build that externalizes a face and forgets its data
# file would previously have deployed and failed at the reader's first page; now it fails here.
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$HERE/web"
BUILD="$HERE/build-wasm"
OUT="$HERE/dist"
PY="env -i PATH=/opt/homebrew/bin:/usr/bin:/bin /usr/bin/python3"

# the faces the product knows about; the engine declares which of them it carries
ALL_FACES="kopub ridibatang"

cd "$HERE"

# --- 1. the engine must be built, and the build must say what it contains ---------------------------
for f in ko_xtch_wasm.js ko_xtch_wasm.wasm ko_build_info.js ko_build_info.json; do
  [ -f "$BUILD/$f" ] || {
    echo "ERROR: $BUILD/$f is missing." >&2
    echo "       configure and build the engine first, e.g." >&2
    echo "         emcmake cmake --preset wasm-production && cmake --build --preset wasm-production" >&2
    exit 1
  }
done

EMBEDDED=$($PY -c "import json;print(' '.join(json.load(open('$BUILD/ko_build_info.json'))['embedded']))")
echo "engine carries : ${EMBEDDED:-（no reader face）}"

# every face the engine does not carry must be shipped as data
EXTERNAL=""
for face in $ALL_FACES; do
  case " $EMBEDDED " in
    *" $face "*) ;;
    *) EXTERNAL="$EXTERNAL $face" ;;
  esac
done
echo "faces as data  : ${EXTERNAL:-（none）}"

# --- 2. sources from web/, engine from the build ----------------------------------------------------
rm -rf "$OUT"
mkdir -p "$OUT"

rsync -a \
  --exclude 'demo.epub' \
  --exclude '.DS_Store' \
  --exclude '.*' \
  --exclude 'ko_xtch_wasm.js' \
  --exclude 'ko_xtch_wasm.wasm' \
  --exclude 'ko_build_info.js' \
  --exclude 'ft_wasm.js' \
  --exclude 'ft_wasm.wasm' \
  --exclude '*.epd2' \
  "$SRC"/ "$OUT"/

cp "$BUILD/ko_xtch_wasm.js" "$BUILD/ko_xtch_wasm.wasm" "$BUILD/ko_build_info.js" "$OUT/"

# FreeType's wasm is built by its own script; it is a generated artifact like the engine
for f in ft_wasm.js ft_wasm.wasm; do
  if [ -f "$BUILD/$f" ]; then
    cp "$BUILD/$f" "$OUT/"
  elif [ -f "$SRC/$f" ]; then
    cp "$SRC/$f" "$OUT/"      # legacy location, until ft_wasm moves under the build tree too
  else
    echo "WARN: $f not found in $BUILD or $SRC — custom-font conversion will be unavailable" >&2
  fi
done

# --- 3. generate the EPD2 blob for each external face ----------------------------------------------
[ -x build/export_external_font ] || {
  echo "ERROR: build/export_external_font is missing (cmake -S . -B build && cmake --build build)" >&2
  exit 1
}
BLOB_FOR_kopub=kopub_14.epd2
BLOB_FOR_ridibatang=ridibatang_14.epd2
for face in $EXTERNAL; do
  case "$face" in
    kopub)      blob="$BLOB_FOR_kopub" ;;
    ridibatang) blob="$BLOB_FOR_ridibatang" ;;
    *) echo "ERROR: no blob name known for face '$face'" >&2; exit 1 ;;
  esac
  echo "exporting      : $face -> $blob"
  build/export_external_font "$OUT/$blob" "$face" >/dev/null || {
    echo "ERROR: could not export $face" >&2
    exit 1
  }
  # Ship it PRE-COMPRESSED, and tell the edge so with a Content-Encoding rule in _headers.
  #
  # Why not let the edge compress it: .epd2 is an unknown extension and Pages serves it as
  # application/octet-stream, which is outside Cloudflare's default compression policy. Measured on a
  # preview deployment of exactly this package: the blob came back byte-for-byte raw (3,169,108
  # bytes) while the .wasm beside it was brotli'd, which made the externalized build transfer 78%
  # MORE than embedding the face.
  #
  # Why q11 rather than a Compression Rule: a zone-level rule is not available for a *.pages.dev
  # hostname, and the edge's dynamic brotli is measurably weaker than -q 11 anyway (45.4% of raw for
  # the module versus 38.8% at q11). So the package carries the better encoding and the edge is asked
  # only to label it.
  #
  # The filename is unchanged, so dev (raw bytes, no _headers) and the package (br bytes plus
  # Content-Encoding) are the same asset to the worker — the browser decodes transparently. If the
  # header is ever NOT applied, the loader sees br bytes where it expects EPD2 and refuses loudly.
  echo "compressing    : $blob (-q 11, $(wc -c < "$OUT/$blob" | tr -d ' ') bytes)"
  brotli -q 11 -c "$OUT/$blob" > "$OUT/$blob.br"
  mv "$OUT/$blob.br" "$OUT/$blob"
done

# --- 4. the commercial book never ships, even if an exclude is edited wrongly -----------------------
if [ -e "$OUT/demo.epub" ]; then
  echo "ERROR: demo.epub leaked into dist/ — refusing to continue" >&2
  exit 1
fi

# --- 5. content-address the generated artifacts and write the manifest ------------------------------
$PY "$HERE/scripts/fingerprint_assets.py" "$OUT"

# --- 6. assert the package describes itself ---------------------------------------------------------
# Fingerprinting renamed the files, so every check below resolves through the manifest.
$PY "$HERE/scripts/verify/package_consistency.py" "$OUT" $EMBEDDED -- $EXTERNAL

echo "built $OUT"
echo "  files: $(find "$OUT" -type f | wc -l | tr -d ' ')   size: $(du -sh "$OUT" | cut -f1)"
ls -1 "$OUT"
