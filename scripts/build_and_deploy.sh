#!/usr/bin/env bash
# Build the wasm engine, refresh web/ + dist/, verify they agree, optionally deploy and verify live.
#
# WHY THIS EXISTS: web/ holds a COPY of the engine artifacts (ko_xtch_wasm.wasm / .js) and
# scripts/build_dist.sh sources from web/. If you rebuild only build-wasm/ and then deploy, you
# ship the STALE wasm and wrangler still reports success. That happened on 2026-09-17: dist was
# refreshed from web/'s pre-PNG-migration wasm (eaac8614...) and uploaded without complaint.
# This script makes that failure impossible to miss or to ship.
#
# Usage: scripts/build_and_deploy.sh [--deploy]
set -euo pipefail
cd "$(dirname "$0")/.."

echo "== build wasm =="
cmake --build build-wasm -j8

BUILD_SHA=$(shasum -a 256 build-wasm/ko_xtch_wasm.wasm | awk '{print $1}')
BUILD_SIZE=$(wc -c < build-wasm/ko_xtch_wasm.wasm | tr -d ' ')
echo "build-wasm : ${BUILD_SHA:0:16}  ${BUILD_SIZE} bytes"

echo "== refresh web/ from the build (the stale-copy guard) =="
cp build-wasm/ko_xtch_wasm.wasm build-wasm/ko_xtch_wasm.js web/

echo "== refresh dist/ =="
bash scripts/build_dist.sh

DIST_SHA=$(shasum -a 256 dist/ko_xtch_wasm.wasm | awk '{print $1}')
if [ "$BUILD_SHA" != "$DIST_SHA" ]; then
  echo "FAIL: dist/ wasm does not match the build"
  echo "      build-wasm : ${BUILD_SHA:0:16}"
  echo "      dist       : ${DIST_SHA:0:16}"
  exit 1
fi
echo "dist       : ${DIST_SHA:0:16}  (matches build)"

if [ "${1:-}" != "--deploy" ]; then
  echo "== no --deploy given: stopping here =="
  exit 0
fi

echo "== deploy =="
npx -y wrangler@latest pages deploy dist --project-name=crosspoint-ko-wasm --branch=main

echo "== verify live artifact =="
LIVE_SHA=$(curl -s "https://crosspoint-ko-wasm.pages.dev/ko_xtch_wasm.wasm?cb=$(date +%s)" | shasum -a 256 | awk '{print $1}')
if [ "$LIVE_SHA" != "$BUILD_SHA" ]; then
  echo "FAIL: live wasm does not match the build"
  echo "      build-wasm : ${BUILD_SHA:0:16}"
  echo "      live       : ${LIVE_SHA:0:16}"
  exit 1
fi
echo "live       : ${LIVE_SHA:0:16}  (matches build)"
echo "OK - build, dist and live all agree."
