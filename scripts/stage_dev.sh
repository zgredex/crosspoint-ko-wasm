#!/usr/bin/env bash
# Stage the engine artifacts into web/ so a plain static server can serve the app from source.
#
# WHY THIS IS SEPARATE FROM build_dist.sh: web/ is SOURCE ONLY — it is tracked, and generated engine
# files must not live in it. That rule exists because they used to, and dist/ sourced from web/, so a
# rebuild of build-wasm/ that was not copied into web/ deployed the OLD engine with no complaint
# (2026-09-17, wasm eaac8614). dist/ now takes the engine straight from build-wasm/; this script
# exists purely so the dev loop (`python3 -m http.server` in web/) keeps working, and it stages the
# same artifacts into the same (now gitignored) paths.
#
# It is a DEV convenience. If you are producing something to deploy, use scripts/build_dist.sh.
#
# usage: scripts/stage_dev.sh [--preset <name>]     (default: leave the build dir as configured)
set -euo pipefail
cd "$(dirname "$0")/.."

if [ "${1:-}" = "--preset" ]; then
  emcmake cmake --preset "$2" >/dev/null
  cmake --build --preset "$2"
fi

for f in ko_xtch_wasm.js ko_xtch_wasm.wasm ko_build_info.js; do
  [ -f "build-wasm/$f" ] || { echo "missing build-wasm/$f — build the engine first" >&2; exit 1; }
  cp "build-wasm/$f" web/
done

# FreeType's wasm is built by its own script
if [ -f build-wasm/ft_wasm.wasm ]; then
  cp build-wasm/ft_wasm.js build-wasm/ft_wasm.wasm web/
fi

# Any face the engine does not carry must be present for the worker to fetch it; a face it DOES carry
# must not be staged, or the dev tree would ship data the engine does not need (the mistake that put
# 5.4 MB of blobs into every visitor's download).
EMBEDDED=$(env -i PATH=/opt/homebrew/bin:/usr/bin:/bin /usr/bin/python3 -c \
  "import json;print(' '.join(json.load(open('build-wasm/ko_build_info.json'))['embedded']))")
for face in kopub ridibatang; do
  case "$face" in
    kopub)      blob=web/kopub_14.epd2 ;;
    ridibatang) blob=web/ridibatang_14.epd2 ;;
  esac
  case " $EMBEDDED " in
    *" $face "*) rm -f "$blob" ;;
    *)           [ -f "$blob" ] || build/export_external_font "$blob" "$face" >/dev/null ;;
  esac
done

echo "staged engine into web/ (gitignored):"
ls -1 web/*.wasm web/*.js web/*.epd2 2>/dev/null | sed 's/^/  /'
echo
echo "serve with:  cd web && python3 -m http.server 8899"
