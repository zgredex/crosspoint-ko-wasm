#!/usr/bin/env bash
# Build the browser-side font converter's FreeType module.
#
# Output: web/ft_wasm.js + web/ft_wasm.wasm (+ a copy in dist/ when dist exists).
# FreeType comes from emscripten's port (-sUSE_FREETYPE=1); the shim is tools/ft_wasm.c.
set -euo pipefail
cd "$(dirname "$0")/.."

: "${EMCC:=emcc}"
command -v "$EMCC" >/dev/null || { echo "emcc not found (install emscripten)" >&2; exit 1; }

EXPORTS='["_ftw_init","_ftw_add_face","_ftw_reset","_ftw_set_char_size","_ftw_char_index","_ftw_wght_range","_ftw_set_wght","_ftw_load_render","_ftw_load_outline","_ftw_embolden","_ftw_bm_width","_ftw_bm_rows","_ftw_bm_left","_ftw_bm_top","_ftw_advance_x","_ftw_size_height","_ftw_size_ascender","_ftw_size_descender","_ftw_bitmap","_ftw_bitmap_is_gray","_ftw_version","_malloc","_free"]'

echo "== building web/ft_wasm.js =="
"$EMCC" tools/ft_wasm.c \
  -O3 -sUSE_FREETYPE=1 \
  -sEXPORTED_FUNCTIONS="$EXPORTS" \
  -sEXPORTED_RUNTIME_METHODS='["ccall","cwrap","HEAPU8","HEAP8","HEAP16","HEAP32","HEAPU32","UTF8ToString"]' \
  -sMODULARIZE=1 -sEXPORT_NAME=createFtConverter \
  -sENVIRONMENT=web,worker,node \
  -sALLOW_MEMORY_GROWTH=1 \
  -sINITIAL_MEMORY=33554432 \
  -sASSERTIONS=0 \
  -o web/ft_wasm.js

ls -la web/ft_wasm.js web/ft_wasm.wasm

# dist/ is a build product copy; refresh it when present so the local dev server and the
# deployed bundle carry the same module.
if [ -d dist ]; then
  cp web/ft_wasm.js web/ft_wasm.wasm dist/
  echo "== copied into dist/ =="
fi

echo "OK"
