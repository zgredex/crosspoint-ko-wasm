#!/usr/bin/env bash
# CrossPoint-KO conformance gate: run the PORT and the PINNED REFERENCE on the same books and
# require the same typography.
#
# WHAT THIS IS NOT: a hash of a file compared against a hash someone wrote down once, and NOT a
# demand for byte-identical device framebuffer planes.
#
# THE CONTRACT IS LAYERED, because "matches CrossPoint-KO" means different things at different
# resolutions:
#
#   LAYER 1 — EXACT (the conformance requirement, every page, integers only)
#     page count, every line's y, every word's x and text and style, ruby, image rects, rules,
#     visible-text offsets, viewport geometry, spec. Enforced as byte-identical layout
#     manifests, so there is no tolerance to argue about.
#
#   LAYER 2a — TEXT RASTER PARITY (MANDATORY, per page)
#     the BW/LSB/MSB planes of every page that contains NO images must be byte-identical to the
#     reference's. The Korean fork owns everything up to and including text rasterisation, so text
#     coverage is not "ours to improve". Image-bearing pages are excluded BY CONTRACT (blue-noise
#     dithering, a different decoder and the preview palette are XTCKO additions past the parity
#     boundary) — which is exactly why this is per page and not per book: one illustration must not
#     be able to hide a text-plane divergence on every other page.
#
#   LAYER 2 — PERCEPTUAL (the conformance requirement for pixels)
#     a page fails only if a reader could SEE the difference as typography: content moved
#     (best integer shift removes the difference), a pixel changed by a visible tone step
#     (>= 2 of 4 levels), or the blurred tone mass shifted. One-level quantization and
#     halftone phase differences pass. Image pages are excluded from the verdict — different
#     decoders and dither models are deliberate — but their numbers are printed.
#
#   LAYER 3 — MECHANICAL (explicitly NOT a requirement)
#     exact BW/LSB/MSB plane bytes, framebuffer packing, e-ink refresh behaviour. Device
#     concerns. Byte equality is measured and printed every run as a diagnostic, and
#     --strict-planes turns it into a failure for like-for-like comparisons.
#
# Controls (a gate that cannot fail is not a gate):
#   * the two binaries must differ (different engine, not the same file twice)
#   * manifests must be non-trivial (pages > 0, lines > 0)
#   * SENSITIVITY: perturbing the reference's glyph advances must make LAYER 1 fail
#     (--sensitivity); scripts/verify/raster_controls.py independently proves LAYER 2 catches
#     a one-column move and a visible tone step while tolerating one-level quantization
#
# usage: oracle_conformance.sh [--quick] [--sensitivity] [--strict-planes]
#                              [--fixtures "a.epub b.epub"] [--font-layers]
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

PORT_BIN=build/ko_xtch_host
ORACLE_BIN=build-oracle/ko_xtch_host
ORACLE_CMAKE_ROOT="${KO_ORACLE_CMAKE_ROOT:-/tmp/up-src/crosspoint-reader-ko-crosspoint-reader-ko-84a3919/lib}"
WORK="${KO_CONFORMANCE_WORK:-/tmp/ko-conformance}"
QUICK=0
SENSITIVITY=0
STRICT_PLANES=0
FONT_LAYERS=1
FIXTURES_OVERRIDE=""
while [ $# -gt 0 ]; do
  case "$1" in
    --quick) QUICK=1 ;;
    --sensitivity) SENSITIVITY=1 ;;
    --strict-planes) STRICT_PLANES=1 ;;
    --no-font-layers) FONT_LAYERS=0 ;;
    --fonts) FONT_LAYERS=1 ;;
    --fixtures) FIXTURES_OVERRIDE="$2"; shift ;;
    *) echo "unknown option $1"; exit 2 ;;
  esac
  shift
done

PASS=0
FAIL=0
note() { printf '  .  %s\n' "$*"; }
ok()   { printf '  OK %s\n' "$*"; }
bad()  { printf '  x  %s\n' "$*"; FAIL=$((FAIL + 1)); }

mkdir -p "$WORK"

# --- 1. the pin must hold before anything is compared -------------------------
echo "== oracle pin =="
if KO_ORACLE_SRC="$ORACLE_CMAKE_ROOT/.." python3 scripts/verify/oracle_pin.py --check | sed 's/^/  /'; then
  ok "vendored engine matches the pin"
else
  bad "oracle pin check failed — refusing to certify against a drifted engine"
  exit 1
fi

# --- 2. builds ----------------------------------------------------------------
echo "== builds =="
[ -x "$PORT_BIN" ] || { bad "missing $PORT_BIN (cmake -S . -B build && cmake --build build)"; exit 1; }
if [ ! -x "$ORACLE_BIN" ]; then
  echo "  building the reference from the pinned checkout..."
  if [ ! -d "$ORACLE_CMAKE_ROOT" ]; then
    bad "no reference checkout at $ORACLE_CMAKE_ROOT — fetch the pinned tarball first (see oracle/README.md)"
    exit 1
  fi
  cmake -S . -B build-oracle -DCMAKE_BUILD_TYPE=Release -DKO_ENGINE_ROOT="$ORACLE_CMAKE_ROOT" >"$WORK/oracle-cmake.log" 2>&1
  cmake --build build-oracle -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 4)" >"$WORK/oracle-build.log" 2>&1 \
    || { bad "reference build failed (see $WORK/oracle-build.log)"; exit 1; }
fi
# CONTROL: the two binaries must not be the same program.
if cmp -s "$PORT_BIN" "$ORACLE_BIN"; then
  bad "port and reference binaries are identical — the comparison would be vacuous"
  exit 1
fi
note "port    $(shasum -a 256 "$PORT_BIN" | cut -c1-16)"
note "oracle  $(shasum -a 256 "$ORACLE_BIN" | cut -c1-16)"

FIXTURES="$FIXTURES_OVERRIDE"
# Fixtures this repo does not ship: the commercial Korean book used for the end-to-end measurement.
# Absent from a clone by design, so the gate skips it with a note instead of failing.
LOCAL_ONLY_FIXTURES="web/demo.epub"
if [ -z "$FIXTURES" ]; then
  # Text fixtures first (their contract is total), then the image-bearing books: those are
  # where layout has to agree across image blocks and page breaks, and where the pixel
  # comparison is the interesting one. ko-symbols carries the codepoints KoPub does not cover, so it
  # is the fixture that would expose any fallback face being drawn during page rendering.
  # NOTE: appended in TWO assignments on purpose. A quoted string continued on the next line is not a
  # continuation — it is a second command, and the first line silently wins, which is how a "full"
  # gate run compared three fixtures and reported PASS for six.
  FIXTURES="oracle/fixtures/ko-text.epub oracle/fixtures/ko-ruby.epub oracle/fixtures/ko-mixed.epub"
  FIXTURES="$FIXTURES oracle/fixtures/ko-symbols.epub web/demo-images.epub web/demo-png.epub web/demo.epub"
  [ "$QUICK" = 1 ] && FIXTURES="oracle/fixtures/ko-text.epub oracle/fixtures/ko-ruby.epub oracle/fixtures/ko-symbols.epub"
fi

# --- 3a. text-plane comparator control ----------------------------------------
# A comparator that cannot fail would report PASS for every book, so the first rendered fixture's planes are
# perturbed by one bit and the comparison is required to catch it.
if [ -d "$WORK/ko-text.port.planes" ]; then
  echo "== control: the text-plane comparator must be able to fail =="
  if python3 scripts/verify/text_plane_parity.py "$WORK/ko-text.port.planes" "$WORK/ko-text.oracle.planes" \
       "$WORK/ko-text.port.json" --self-control 2>&1 | sed 's/^/  /'; then
    ok "text-plane comparator control"
  else
    bad "text-plane comparator did not catch a flipped bit — the layer above certifies nothing"
  fi
fi

# --- 3. sensitivity control ---------------------------------------------------
if [ "$SENSITIVITY" = 1 ]; then
  echo "== sensitivity: perturb the reference's KoPub glyph advances =="
  # EVERY glyph's advanceX moves by +63 (= +3.9 px at 16ths). Deliberately a blunt
  # instrument: the question is only whether a metric change reaches the layout at all, and
  # a single-glyph nudge can miss every line of a fixture. The edit count is asserted, so a
  # regex that stops matching fails the control instead of passing it.
  PERT_BIN="$WORK/ko_xtch_host_perturbed"
  PERT_ROOT="$WORK/perturbed-engine"
  if [ ! -f "$PERT_BIN" ]; then
    mkdir -p "$PERT_ROOT"
    cp -R "$ORACLE_CMAKE_ROOT/." "$PERT_ROOT/"
    # TextBlock.cpp and ChapterHtmlSlimParser.cpp include "../../../../src/fontIds.h" —
    # a RELATIVE path that expects the engine to sit four levels below a directory holding
    # src/. That holds for vendor-lib/ and for the reference checkout; a copy anywhere else
    # must bring that directory along or the build fails with a missing-header error.
    ln -sfn "$(cd "$ORACLE_CMAKE_ROOT/../src" && pwd)" "$WORK/src"
    python3 - "$PERT_ROOT/EpdFont/builtinFonts/kopub_14_regular.h" <<'PY'
import re, sys
path = sys.argv[1]
src = open(path, encoding='utf-8').read()
# Glyph records are line-oriented and carry their codepoint: { a, w, h, l, t, ay, off }, // U+XXXX
pattern = re.compile(r'^(?P<indent>\s*)\{\s*(?P<adv>\d+)(?P<rest>,\s*\d+,\s*\d+,\s*\d+,\s*\d+,\s*\d+,\s*\d+\s*\},.*//\s*U\+)',
                     re.M)
edits = [0]
def bump(m):
    edits[0] += 1
    return f"{m.group('indent')}{{ {int(m.group('adv')) + 63}{m.group('rest')}"
out = pattern.sub(bump, src)
if edits[0] < 1000:
    raise SystemExit(f'only {edits[0]} glyph records perturbed — the table shape changed, control is void')
open(path, 'w', encoding='utf-8').write(out)
print(f'perturbed {edits[0]} glyph advances by +63/16 px')
PY
    [ $? -eq 0 ] || { bad "could not perturb the reference font table"; exit 1; }
    cmake -S . -B build-oracle-perturbed -DCMAKE_BUILD_TYPE=Release -DKO_ENGINE_ROOT="$PERT_ROOT" >/dev/null 2>&1
    cmake --build build-oracle-perturbed -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 4)" >"$WORK/perturbed-build.log" 2>&1 \
      || { bad "perturbed reference build failed (see $WORK/perturbed-build.log)"; exit 1; }
    cp build-oracle-perturbed/ko_xtch_host "$PERT_BIN"
  fi
  FIX="oracle/fixtures/ko-text.epub"
  "$PORT_BIN" "$REPO_ROOT/$FIX" "$WORK/port.xtch" --manifest "$WORK/port.json" >/dev/null 2>&1
  "$PERT_BIN" "$REPO_ROOT/$FIX" "$WORK/pert.xtch" --manifest "$WORK/pert.json" >/dev/null 2>&1
  if cmp -s "$WORK/port.json" "$WORK/pert.json"; then
    bad "SENSITIVITY: a +63/16 px advance change on the first glyph did NOT change the layout manifest — the gate cannot detect a metric divergence"
  else
    ok "SENSITIVITY: perturbed metrics change the manifest (gate can fail)"
    python3 scripts/verify/layout_diff.py "$WORK/port.json" "$WORK/pert.json" | sed 's/^/     /' | head -8
  fi
  exit $((FAIL > 0))
fi

# --- 4. fixtures --------------------------------------------------------------
for fx in $FIXTURES; do
  # A fixture that is absent BY DESIGN (a commercial book this repo does not redistribute) is not a
  # gate failure; a fixture that should be in the checkout and is not, is. Without this distinction a
  # fresh clone reported "missing fixture web/demo.epub" as a failure — technically true, and wrong
  # about what it means, which is the kind of message that teaches people to ignore the gate.
  if [ ! -f "$fx" ]; then
    case " $LOCAL_ONLY_FIXTURES " in
      *" $fx "*)
        note "skipped $fx — local-only fixture (not redistributable, so not in the repo)"
        continue ;;
    esac
    bad "missing fixture $fx"
    continue
  fi
  name="$(basename "$fx" .epub)"
  echo "== $fx =="
  # --dump-planes rides along with the render that is already happening, so the per-page text-raster
  # comparison below costs a directory walk rather than a second render.
  "$PORT_BIN" "$REPO_ROOT/$fx" "$WORK/$name.port.xtch" --manifest "$WORK/$name.port.json" \
    --dump-planes "$WORK/$name.port.planes" \
    >"$WORK/$name.port.log" 2>&1 || { bad "port failed on $fx (see $WORK/$name.port.log)"; continue; }
  "$ORACLE_BIN" "$REPO_ROOT/$fx" "$WORK/$name.oracle.xtch" --manifest "$WORK/$name.oracle.json" \
    --dump-planes "$WORK/$name.oracle.planes" \
    >"$WORK/$name.oracle.log" 2>&1 || { bad "reference failed on $fx (see $WORK/$name.oracle.log)"; continue; }

  # CONTROL: a manifest that carries no pages certifies nothing.
  pages=$(python3 -c "import json,sys;print(len(json.load(open(sys.argv[1]))['pages']))" "$WORK/$name.port.json")
  lines=$(python3 -c "import json,sys;print(sum(len(p['lines']) for p in json.load(open(sys.argv[1]))['pages']))" "$WORK/$name.port.json")
  if [ "$pages" -lt 1 ] || [ "$lines" -lt 1 ]; then
    bad "$name: manifest carries $pages page(s)/$lines line(s) — nothing was compared"
    continue
  fi

  # LAYER 1 — EXACT, every page. The layout decisions themselves, as integers: page count,
  # every line's y, every word's x, word text, styles, ruby, image rects, rules, visible-text
  # offsets, viewport and spec. Byte-identical manifests, and it is the strongest of the three
  # layers precisely because nothing here is a tolerance.
  if cmp -s "$WORK/$name.port.json" "$WORK/$name.oracle.json"; then
    ok "$name: LAYER 1 exact — layout manifest identical ($pages pages, $lines lines)"
  else
    bad "$name: LAYER 1 FAILED — layout manifest differs"
    python3 scripts/verify/layout_diff.py "$WORK/$name.port.json" "$WORK/$name.oracle.json" | head -14 | sed 's/^/     /'
    continue
  fi

  # LAYER 2a — TEXT RASTER PARITY: MANDATORY, per page.
  # The Korean fork owns everything up to and including text rasterisation, so on a page with no images
  # the BW/LSB/MSB planes must be byte-identical to the reference's. Image-bearing pages are the ones
  # where XTCKO is allowed to differ (blue-noise dithering, a different decoder), so they are reported
  # and never judged. Whole-container equality cannot express that distinction: one illustration would
  # hide a text divergence on every other page of the book.
  if python3 scripts/verify/text_plane_parity.py "$WORK/$name.port.planes" "$WORK/$name.oracle.planes" \
       "$WORK/$name.port.json" 2>&1 | sed 's/^/     /'; then
    ok "$name: LAYER 2a text raster parity — every image-free page's planes are byte-identical"
  else
    bad "$name: LAYER 2a FAILED — a TEXT page's planes differ from the reference"
  fi

  # The mechanical figure, reported and NOT gated (see --strict-planes). Byte equality is
  # achievable and currently holds on text pages; it is a diagnostic because the contract is
  # typographic, and a browser-side rasterizer is not required to reproduce device plane bytes.
  if cmp -s "$WORK/$name.port.xtch" "$WORK/$name.oracle.xtch"; then
    note "$name: containers byte-identical (all planes, all pages) — reported, not required"
  else
    python3 scripts/verify/container_diff.py "$WORK/$name.oracle.xtch" "$WORK/$name.port.xtch" 2>&1 \
      | head -4 | sed 's/^/     /'
  fi

  # LAYER 2 — PERCEPTUAL. A page fails only if a reader could see the difference as
  # typography: content moved, or a pixel changed by a visible tone step, or the tone mass
  # shifted. One-level quantization and halftone phase differences pass. Image pages are
  # excluded from the verdict (different decoders and dither models by design) but their
  # numbers are still printed.
  raster_args="--skip-images $WORK/$name.port.json --limit 4"
  if [ "$QUICK" != 1 ] && [ "$pages" -gt 60 ]; then
    # ~2 s/page in Python; the exact layer above already covers every page, so sample evenly
    # rather than truncating (a regression in the last chapter must not slip through).
    raster_args="$raster_args --sample 40"
  fi
  if python3 scripts/verify/raster_diff.py "$WORK/$name.oracle.xtch" "$WORK/$name.port.xtch" \
       $raster_args 2>&1 | sed 's/^/     /'; then
    ok "$name: LAYER 2 perceptual — nothing moved, no visible tone step"
  else
    bad "$name: LAYER 2 FAILED — a raster difference that is visible as typography"
  fi

  # LAYER 3 — not a requirement, and it is the reason this gate has a layered shape at all:
  # device framebuffer packing, refresh behaviour and plane representation are the device's
  # concern. Kept as an opt-in so a like-for-like host-vs-host comparison can still ask for it.
  if [ "$STRICT_PLANES" = 1 ]; then
    if cmp -s "$WORK/$name.port.xtch" "$WORK/$name.oracle.xtch"; then
      ok "$name: LAYER 3 strict planes — byte-identical"
    else
      bad "$name: LAYER 3 strict planes — planes differ (--strict-planes was requested)"
    fi
  fi
done

if [ "$FONT_LAYERS" = 1 ]; then
  echo "== KoPub externalization, in the same three layers =="
  # The font leaves the wasm as an EPD2 blob (src/external_font_blob.h): the runtime arrays
  # relocated verbatim, no re-rasterisation and no quantization, because the metrics ARE the
  # layout. What has to hold, in the layering this project uses:
  #
  #   metrics   exact — tools/verify_external_font.cpp compares the blob against the embedded
  #             arrays field by field: every advanceX bit-identical (U+AC00 = 437 = 27.3125 px,
  #             not 27<<4), the whole kern matrix, the interval and glyph records, the bitmaps
  #   LAYER 1   exact — embedded vs externalized must produce byte-identical layout manifests
  #   LAYER 2   perceptual — and since the same metric data is in play, byte-identical pixels
  #   LAYER 3   not a requirement
  BLOB="${KO_KOPUB_BLOB:-/tmp/ab/kopub_14.epd2}"
  if [ ! -f "$BLOB" ] && [ -x build/export_external_font ]; then
    build/export_external_font "$BLOB" kopub >/dev/null 2>&1
    note "exported $BLOB"
  fi
  # Both faces, because the container is generic and the optional face is the one whose
  # externalization is actually on the table: RIDIBatang costs the default wasm 267,999 Brotli
  # bytes it does not need to carry, so its EPD2 path has to be as provable as KoPub's. It was not
  # even exportable until the tool's KoPub-only constants were replaced with per-face expectations.
  for FACE in kopub ridibatang; do
    BLOB="${KO_FACE_BLOB_DIR:-/tmp/ab}/${FACE}_14.epd2"
    [ "$FACE" = "kopub" ] && BLOB="${KO_KOPUB_BLOB:-/tmp/ab/kopub_14.epd2}"
    [ "$FACE" = "ridibatang" ] && BLOB="${KO_RIDI_BLOB:-/tmp/ab/ridi_14.epd2}"
    if [ ! -f "$BLOB" ] && [ -x build/export_external_font ]; then
      build/export_external_font "$BLOB" "$FACE" >/dev/null 2>&1
      note "exported $BLOB"
    fi
    echo "-- $FACE --"
    if [ ! -f "$BLOB" ]; then
      bad "$FACE: no EPD2 blob at $BLOB and no build/export_external_font to make one"
      continue
    fi
    # Layer "metrics exact", first and on its own: the blob must reproduce the embedded face bit for
    # bit — for KoPub including U+AC00's 437 (27.3125 px) advance and the full kern matrix, for
    # RIDIBatang a kern-less blob that says so. If this passes, the layers below are a smoke test;
    # if it fails, they are how you find the damage.
    if [ -x build/verify_external_font ]; then
      if build/verify_external_font "$BLOB" "$FACE" >"$WORK/font-$FACE-metrics.log" 2>&1; then
        ok "font($FACE): metrics exact — $(tail -1 "$WORK/font-$FACE-metrics.log")"
      else
        bad "font($FACE): metrics FAILED — the blob does not reproduce the embedded face"
        tail -6 "$WORK/font-$FACE-metrics.log" | sed 's/^/     /'
      fi
    else
      bad "build/verify_external_font is not built — the metrics layer would go unchecked"
    fi
    FX=oracle/fixtures/ko-text.epub
    "$PORT_BIN" "$REPO_ROOT/$FX" "$WORK/font-$FACE-embedded.xtch" --font "$FACE" \
      --manifest "$WORK/font-$FACE-embedded.json" >"$WORK/font-$FACE-embedded.log" 2>&1
    if "$PORT_BIN" "$REPO_ROOT/$FX" "$WORK/font-$FACE-external.xtch" --font "$FACE" \
         --external-font "$FACE" "$BLOB" --manifest "$WORK/font-$FACE-external.json" \
         >"$WORK/font-$FACE-external.log" 2>&1; then
      note "$(grep -o "$FACE registered from blob.*" "$WORK/font-$FACE-external.log" | head -1)"
    else
      bad "port failed with --external-font $FACE (see $WORK/font-$FACE-external.log)"
    fi
    if [ ! -s "$WORK/font-$FACE-external.json" ]; then
      bad "font($FACE): no manifest from the externalized run — nothing was compared"
    elif cmp -s "$WORK/font-$FACE-embedded.json" "$WORK/font-$FACE-external.json"; then
      ok "font($FACE): LAYER 1 exact — externalized face lays out identically (metrics survived)"
    else
      bad "font($FACE): LAYER 1 FAILED — externalizing changed the layout; the blob is lossy"
      python3 scripts/verify/layout_diff.py "$WORK/font-$FACE-embedded.json" \
        "$WORK/font-$FACE-external.json" | head -8 | sed 's/^/     /'
    fi
    if python3 scripts/verify/raster_diff.py "$WORK/font-$FACE-embedded.xtch" \
         "$WORK/font-$FACE-external.xtch" --limit 3 2>&1 | sed 's/^/     /'; then
      ok "font($FACE): LAYER 2 perceptual — externalized rendering is visually identical"
    else
      bad "font($FACE): LAYER 2 FAILED — the externalized face renders differently"
    fi
    if cmp -s "$WORK/font-$FACE-embedded.xtch" "$WORK/font-$FACE-external.xtch"; then
      note "font($FACE): containers byte-identical too (expected — EPD2 stores no quantized copy)"
    fi
  done
fi

echo
if [ "$FAIL" -eq 0 ]; then
  echo "PASS — port and reference agree at the level this gate claims"
else
  echo "FAIL — $FAIL check(s) failed"
fi
exit $((FAIL > 0))
