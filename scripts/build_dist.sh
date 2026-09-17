#!/usr/bin/env bash
# Build the deployable static bundle for Cloudflare Pages (or any static host).
#
# Why this exists instead of "just point the host at web/": web/demo.epub is a
# commercial, non-redistributable test book (see .gitignore). It lives on disk
# for local testing but must never be uploaded. Building dist/ with an explicit
# exclude list makes that structural rather than a thing someone has to remember.
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$HERE/web"
OUT="$HERE/dist"

rm -rf "$OUT"
mkdir -p "$OUT"

rsync -a \
  --exclude 'demo.epub' \
  --exclude '.DS_Store' \
  --exclude '.*' \
  "$SRC"/ "$OUT"/

# refuse to ship the commercial book even if someone renames/excludes wrongly
if [ -e "$OUT/demo.epub" ]; then
  echo "ERROR: demo.epub leaked into dist/ — refusing to continue" >&2
  exit 1
fi

echo "built $OUT"
echo "  files: $(find "$OUT" -type f | wc -l | tr -d ' ')   size: $(du -sh "$OUT" | cut -f1)"
ls -1 "$OUT"
