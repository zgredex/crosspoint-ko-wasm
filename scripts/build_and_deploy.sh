#!/usr/bin/env bash
# Build the wasm engine, package dist/, verify the package against the build, optionally deploy and
# verify the live artifacts.
#
# WHY THIS EXISTS: web/ used to hold a COPY of the engine artifacts and build_dist.sh sourced from
# web/. If you rebuilt only build-wasm/ and deployed, you shipped the STALE wasm and wrangler still
# reported success. That happened on 2026-09-17: dist was refreshed from web/'s pre-PNG-migration wasm
# (eaac8614...) and uploaded without complaint. web/ is now source-only and dist/ takes the engine
# straight from build-wasm/, so there is no copy to go stale.
#
# Usage: scripts/build_and_deploy.sh [--preset <name>] [--deploy]
#        default preset: wasm-production   (variant C: no reader face embedded)
set -euo pipefail
cd "$(dirname "$0")/.."

PRESET=wasm-production
DEPLOY=0
while [ $# -gt 0 ]; do
  case "$1" in
    --preset) PRESET="$2"; shift 2 ;;
    --deploy) DEPLOY=1; shift ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

# The configuration is set EXPLICITLY, from a preset, every time. Inheriting whatever an existing
# build directory happened to be configured as is how a dev build (both faces embedded) becomes the
# production deployment without anyone deciding that.
echo "== configure ($PRESET) =="
emcmake cmake --preset "$PRESET" >/dev/null

echo "== build wasm =="
cmake --build --preset "$PRESET"

# The EPD2 blobs come from the native font tool, so it must exist before packaging.
echo "== host font tool =="
if [ ! -x build/export_external_font ]; then
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build build -j8 --target export_external_font
fi

BUILD_SHA=$(shasum -a 256 build-wasm/ko_xtch_wasm.wasm | awk '{print $1}')
BUILD_SIZE=$(wc -c < build-wasm/ko_xtch_wasm.wasm | tr -d ' ')
echo "build-wasm : ${BUILD_SHA:0:16}  ${BUILD_SIZE} bytes"
echo "carries    : $(cat build-wasm/ko_build_info.json | tr -d ' \n')"

echo "== package dist/ =="
bash scripts/build_dist.sh

# the package must contain the very engine that was just built
DIST_WASM=$(env -i PATH=/opt/homebrew/bin:/usr/bin:/bin /usr/bin/python3 -c \
  "import json;print(json.load(open('dist/assets.json'))['ko_xtch_wasm.wasm'])")
DIST_SHA=$(shasum -a 256 "dist/$DIST_WASM" | awk '{print $1}')
if [ "$BUILD_SHA" != "$DIST_SHA" ]; then
  echo "FAIL: dist/ wasm does not match the build" >&2
  echo "      build-wasm : ${BUILD_SHA:0:16}" >&2
  echo "      dist       : ${DIST_SHA:0:16}" >&2
  exit 1
fi
echo "dist       : ${DIST_SHA:0:16}  (matches build)"

# and the declaration the worker will read must match the engine that was built, not just exist
CANON=$(shasum -a 256 build-wasm/ko_build_info.js | awk '{print $1}')
DIST_INFO=$(env -i PATH=/opt/homebrew/bin:/usr/bin:/bin /usr/bin/python3 -c \
  "import json;print(json.load(open('dist/assets.json'))['ko_build_info.js'])")
DIST_INFO_SHA=$(shasum -a 256 "dist/$DIST_INFO" | awk '{print $1}')
if [ "$CANON" != "$DIST_INFO_SHA" ]; then
  echo "FAIL: packaged ko_build_info.js is not the one this engine was built with" >&2
  exit 1
fi
echo "build info : ${DIST_INFO_SHA:0:16}  (matches build)"

if [ "$DEPLOY" != 1 ]; then
  echo "== no --deploy given: stopping here =="
  exit 0
fi

echo "== deploy =="
npx -y wrangler@latest pages deploy dist --project-name=crosspoint-ko-wasm --branch=main

# ------------------------------------------------------------------------------------------------
# Live verification. Two things are checked, because they fail differently:
#   1. the site is serving the engine and declaration that were just built
#   2. any externalized face is actually Brotli-compressed AT THE EDGE
# (2) is not cosmetic: .epd2 is an unknown extension, and Cloudflare's default compression policy
# works off content type. If the edge serves the blob uncompressed, an externalized-face build
# transfers ~3.1 MB for KoPub instead of ~770 KB and is WORSE than embedding it. So the package is
# not considered deployable until the edge has been asked.
# ------------------------------------------------------------------------------------------------
HOST=https://crosspoint-ko-wasm.pages.dev
CB=$(date +%s)

echo "== verify live engine =="
LIVE_WASM=$(curl -s "$HOST/assets.json?cb=$CB" \
  | sed -n 's/.*"ko_xtch_wasm.wasm"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p')
LIVE_INFO=$(curl -s "$HOST/assets.json?cb=$CB" \
  | sed -n 's/.*"ko_build_info.js"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p')
if [ -z "$LIVE_WASM" ]; then
  echo "FAIL: no manifest at $HOST/assets.json — cannot verify what is live" >&2
  exit 1
fi
# A GET, not -I: the headers of the response we actually serve are the thing being verified.
LIVE_SHA=$(curl -s "$HOST/${LIVE_WASM}?cb=$CB" | shasum -a 256 | awk '{print $1}')
if [ "$LIVE_SHA" != "$BUILD_SHA" ]; then
  echo "FAIL: live wasm does not match the build" >&2
  echo "      build-wasm : ${BUILD_SHA:0:16}" >&2
  echo "      live       : ${LIVE_SHA:0:16}" >&2
  echo "      (a CDN alias can lag a fresh deployment by a few seconds; re-run to confirm)" >&2
  exit 1
fi
echo "live wasm  : ${LIVE_SHA:0:16}  (matches build)"

[ -n "$LIVE_INFO" ] && echo "live info  : $LIVE_INFO"

echo "== verify edge compression of every externalized face =="
FAILED_COMPRESSION=0
for blob in $(env -i PATH=/opt/homebrew/bin:/usr/bin:/bin /usr/bin/python3 -c \
      "import json;a=json.load(open('dist/assets.json'));print(' '.join(v for k,v in a.items() if k.endswith('.epd2')))"); do
  RAW=$(wc -c < "dist/$blob" | tr -d ' ')
  printf '  %-30s ' "$blob"
  RESP=$(curl -sS -H 'Accept-Encoding: br' -D /tmp/edge-headers -o /dev/null \
         -w '%{size_download}' "$HOST/$blob?cb=$CB")
  if grep -qi '^content-encoding: br' /tmp/edge-headers; then
    echo "br  ${RESP} bytes on the wire (package ${RAW})"
  else
    echo "NOT COMPRESSED  ${RESP} bytes on the wire (package ${RAW})"
    sed -n '1p;/[Cc]ontent-[Tt]ype/p' /tmp/edge-headers | sed 's/^/      /'
    FAILED_COMPRESSION=1
  fi
done

if [ "$FAILED_COMPRESSION" = 1 ]; then
  echo >&2
  echo "FAIL: an externalized face is not Brotli-compressed at the edge." >&2
  echo "      With this size ratio the externalized build is worse than embedding the face." >&2
  echo "      Fix with a Cloudflare Compression Rule matching *.epd2 and selecting Brotli, then" >&2
  echo "      re-run this verification (do NOT rename the blobs to fake a compressible MIME type)." >&2
  exit 1
fi

echo "OK - build, dist and live agree; every externalized face is Brotli at the edge."
