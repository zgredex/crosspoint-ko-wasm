#!/usr/bin/env bash
# CrossPoint-KO conformance gate: run the PORT and the PINNED REFERENCE on the same books
# and require the same layout, and the same pixels where pixels are comparable.
#
# WHAT THIS IS NOT: a hash of a file compared against a hash someone wrote down once. The
# reference is compiled from its own sources at the pinned commit and executed, so a
# difference in behaviour has nowhere to hide — including in a function the port added, a
# header it amended, or a fast path it introduced.
#
# Contract enforced (per fixture, all must hold):
#   1. page count           identical
#   2. layout manifest      byte-identical (lines, line y, word x, word text, styles, ruby,
#                           image rects, rules, visible-text offsets, spec, viewport)
#   3. text pages           BW/LSB/MSB planes byte-identical
#   4. image pages          layout identical; pixels EXEMPT and reported, because the
#                           reference refuses oversized JPEGs as a device RAM policy while
#                           the port renders them (see docs/ko-jpeg-libjpeg-port.md)
#
# Controls (a gate that cannot fail is not a gate):
#   * the two binaries must differ (different engine, not the same file twice)
#   * the layout manifests must be non-trivial (pages > 0, and the text fixture must
#     contain lines) — an empty manifest compares equal to an empty manifest
#   * SENSITIVITY: with one glyph advance perturbed in the reference's KoPub table, the
#     gate must FAIL. Run with --sensitivity to execute just this.
#
# usage: oracle_conformance.sh [--quick] [--sensitivity] [--fixtures "a.epub b.epub"]
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

PORT_BIN=build/ko_xtch_host
ORACLE_BIN=build-oracle/ko_xtch_host
ORACLE_CMAKE_ROOT="${KO_ORACLE_CMAKE_ROOT:-/tmp/up-src/crosspoint-reader-ko-crosspoint-reader-ko-84a3919/lib}"
WORK="${KO_CONFORMANCE_WORK:-/tmp/ko-conformance}"
QUICK=0
SENSITIVITY=0
FIXTURES_OVERRIDE=""
while [ $# -gt 0 ]; do
  case "$1" in
    --quick) QUICK=1 ;;
    --sensitivity) SENSITIVITY=1 ;;
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
if [ -z "$FIXTURES" ]; then
  # Text fixtures first (their contract is total), then the image-bearing books: those are
  # where layout has to agree across image blocks and page breaks, and where the pixel
  # exemption is exercised rather than assumed.
  FIXTURES="oracle/fixtures/ko-text.epub oracle/fixtures/ko-ruby.epub oracle/fixtures/ko-mixed.epub web/demo-images.epub web/demo-png.epub web/demo.epub"
  [ "$QUICK" = 1 ] && FIXTURES="oracle/fixtures/ko-text.epub oracle/fixtures/ko-ruby.epub"
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
  [ -f "$fx" ] || { bad "missing fixture $fx"; continue; }
  name="$(basename "$fx" .epub)"
  echo "== $fx =="
  "$PORT_BIN" "$REPO_ROOT/$fx" "$WORK/$name.port.xtch" --manifest "$WORK/$name.port.json" \
    >"$WORK/$name.port.log" 2>&1 || { bad "port failed on $fx (see $WORK/$name.port.log)"; continue; }
  "$ORACLE_BIN" "$REPO_ROOT/$fx" "$WORK/$name.oracle.xtch" --manifest "$WORK/$name.oracle.json" \
    >"$WORK/$name.oracle.log" 2>&1 || { bad "reference failed on $fx (see $WORK/$name.oracle.log)"; continue; }

  # CONTROL: a manifest that carries no pages certifies nothing.
  pages=$(python3 -c "import json,sys;print(len(json.load(open(sys.argv[1]))['pages']))" "$WORK/$name.port.json")
  lines=$(python3 -c "import json,sys;print(sum(len(p['lines']) for p in json.load(open(sys.argv[1]))['pages']))" "$WORK/$name.port.json")
  if [ "$pages" -lt 1 ] || [ "$lines" -lt 1 ]; then
    bad "$name: manifest carries $pages page(s)/$lines line(s) — nothing was compared"
    continue
  fi

  if cmp -s "$WORK/$name.port.json" "$WORK/$name.oracle.json"; then
    ok "$name: layout manifest identical ($pages pages, $lines lines)"
  else
    bad "$name: layout manifest differs"
    python3 scripts/verify/layout_diff.py "$WORK/$name.port.json" "$WORK/$name.oracle.json" | head -14 | sed 's/^/     /'
    continue
  fi

  # Planes. Pixels on pages that carry an image are exempt (the port's decode+dither path is
  # deliberately its own); everything else must match. The rule is applied whenever the
  # containers differ, whether or not the reference refused an image — refusal is a REASON to
  # print, never a precondition for looking. Without this, an image book whose pixels differ
  # for the dither/decoder reason alone would be rejected for lack of a refusal, which is a
  # gate failing on the wrong grounds.
  refused=$(grep -c "Image too large" "$WORK/$name.oracle.log" || true)
  if cmp -s "$WORK/$name.port.xtch" "$WORK/$name.oracle.xtch"; then
    ok "$name: containers byte-identical (all planes, all pages)"
  else
    summary=$(python3 scripts/verify/container_diff.py "$WORK/$name.oracle.xtch" "$WORK/$name.port.xtch" 2>&1)
    echo "$summary" | head -4 | sed 's/^/     /'
    # Indices are GLOBAL container page numbers, in the same order the manifest was recorded,
    # so a text page cannot hide behind a per-spine page number that happens to be reused by an
    # image page in another chapter. The alignment itself is asserted: if the manifest and the
    # containers disagree on how many pages exist, the comparison is meaningless and says so
    # instead of passing.
    python3 - "$WORK/$name.port.json" "$WORK/$name.oracle.xtch" "$WORK/$name.port.xtch" <<'PY'
import json, sys
sys.path.insert(0, 'scripts/verify')
import container_diff as cd
manifest = json.load(open(sys.argv[1]))
ap, _ = cd.container(sys.argv[2])
bp, _ = cd.container(sys.argv[3])
pages = manifest['pages']
if not (len(pages) == len(ap) == len(bp)):
    print(f'  x  page-count mismatch: manifest {len(pages)}, containers {len(ap)}/{len(bp)}')
    sys.exit(2)
img_idx = {i for i, p in enumerate(pages) if p['images']}
diffs = set()
for i in range(len(pages)):
    a, b = cd.planes(ap[i]), cd.planes(bp[i])
    if any(x != y for (x, _n), (y, _m) in zip(a, b)):
        diffs.add(i)
text_diffs = sorted(diffs - img_idx)
print(f'     differing pages: {len(diffs)} of {len(pages)}; '
      f'every one carries an image: {diffs <= img_idx}')
if text_diffs:
    print(f'  x  {len(text_diffs)} page(s) WITHOUT an image differ: '
          f'{[pages[i]["spine"] for i in text_diffs[:6]]}/{[pages[i]["page"] for i in text_diffs[:6]]}')
    sys.exit(1)
sys.exit(0)
PY
    dup=$?
    if [ "$dup" -eq 0 ]; then
      ok "$name: every text page byte-identical; ${refused} reference image refusal(s), image pixels exempt"
    else
      bad "$name: a page WITHOUT an image differs — that is a pixel divergence, not policy"
    fi
  fi
done

echo
if [ "$FAIL" -eq 0 ]; then
  echo "PASS — port and reference agree at the level this gate claims"
else
  echo "FAIL — $FAIL check(s) failed"
fi
exit $((FAIL > 0))
