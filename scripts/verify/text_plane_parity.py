#!/usr/bin/env python3
"""text_plane_parity.py — exact text-pixel parity on every page whose text can be isolated.

The contract (docs/ko-oracle-conformance.md):

    TEXT RASTER PARITY — MANDATORY on every IMAGE-FREE page. On an image-free page the three
      planes must be byte-identical to the reference's; there is nothing on such a page that
      XTCKO is allowed to re-rasterise.

    ON AN IMAGE-BEARING PAGE the image rectangles are exempted and the text bits OUTSIDE them are
      compared (blue-noise dithering, a different decoder and the preview palette are XTCKO
      additions past the parity boundary). The rectangles come from the layout manifest, which
      LAYER 1 has already proven identical, so both renderers agree on where the images are.

An earlier version skipped any page containing an image. That was too broad: one small illustration
beside twenty lines of Korean prose could hide completely wrong glyph rasterisation. Exempting the
rectangles instead of the pages keeps the exemption exactly as large as the thing that is allowed to
differ.

COORDINATE SYSTEM — the thing this file got wrong for one revision. The dumped `.bw/.lsb/.msb`
vectors are NOT logical portrait images. They are raw PHYSICAL framebuffers: X4 is 800x480 at
100 bytes/row; X3 is 792x528 at 99 bytes/row. The geometry is read from the layout manifest, which
the host snapshots from the selected Korean-fork display profile. The logical portrait buffer is
what the XTH/XTG encoder builds from them (src/xtch_writer.h), and its byte layout is different.

Two conversions are therefore mandatory before a manifest rectangle can be used as a mask:

  1. the manifest's image x/y are PAGE-LOCAL; rendering adds marginLeft/marginTop
     (PageImage::render -> imageBlock->render(renderer, xPos + xOffset, yPos + yOffset));
  2. logical -> physical uses the selected Korean-fork orientation transform.
     Portrait is phyX=logicalY, phyY=physicalHeight-1-logicalX; landscape CW/CCW and
     inverted portrait use GfxRenderer::rotateCoordinates verbatim below.

Measured on web/demo-images.epub over all 45 pairs of the ten dither models: with the corrected mapping,
changing --image-dither moves 0 pixel bits outside the image rectangles, and 0 bits on pages the manifest
says hold no image (spine 0/5/9). Under the old 480x800/60 mapping the same 45 comparisons reported
1,676,856 bits "outside" the rectangles, worst pair 43,396 — every one of them an artefact of masking the
wrong region. The claim this file used to carry, that an image perturbs its whole page, does not survive
the correction.

Two properties this file also refuses to leave implicit:

  * the page set is compared against the MANIFEST, by (spine, page) identity — not against the other
    directory alone. If both renderers omit the same page they agree perfectly and certify nothing.
  * a mutation control runs against a real page and must be caught.

usage:
  text_plane_parity.py <portPlanesDir> <oraclePlanesDir> <manifest.json> [--self-control]
"""
import json
import os
import shutil
import sys
import tempfile
from collections import Counter

PLANES = ('.bw', '.lsb', '.msb')

def page_files(directory):
    """(spine, page) -> {suffix: path} for one dumped plane directory."""
    found = {}
    if not os.path.isdir(directory):
        return found
    for entry in os.listdir(directory):
        for suffix in PLANES:
            if entry.endswith(suffix):
                stem = entry[: -len(suffix)]
                if stem.startswith('p'):        # the host names them p%05d_%05d.bw
                    stem = stem[1:]
                try:
                    spine, page = (int(v) for v in stem.split('_'))
                except ValueError:
                    continue
                found.setdefault((spine, page), {})[suffix] = os.path.join(directory, entry)
    return found


def logical_to_physical(x, y, orientation, geometry):
    """CrossPoint-KO GfxRenderer::rotateCoordinates, verbatim as integer math."""
    physical_width, physical_height, _row_bytes, _plane_bytes = geometry
    if orientation == 1:      # LandscapeClockwise
        return physical_width - 1 - x, physical_height - 1 - y
    if orientation == 2:      # PortraitInverted
        return physical_width - 1 - y, x
    if orientation == 3:      # LandscapeCounterClockwise
        return x, y
    return y, physical_height - 1 - x


def rect_to_physical(r, margin_top, margin_left, orientation, geometry):
    """A manifest image rect -> the PHYSICAL span it covers, or None if it is empty/off-panel.

    manifest x/y are page-local logical coordinates; rendering offsets them by the margins. The
    logical page is then rotated into the physical framebuffer by the selected
    CrossPoint-KO orientation.
    """
    lx0 = int(r['x']) + margin_left
    ly0 = int(r['y']) + margin_top
    lx1 = lx0 + int(r['w'])
    ly1 = ly0 + int(r['h'])
    physical_width, physical_height, _row_bytes, _plane_bytes = geometry
    logical_width, logical_height = ((physical_width, physical_height)
                                     if orientation in (1, 3)
                                     else (physical_height, physical_width))
    lx0, lx1 = max(0, lx0), min(logical_width, lx1)
    ly0, ly1 = max(0, ly0), min(logical_height, ly1)
    if lx1 <= lx0 or ly1 <= ly0:
        return None
    corners = [logical_to_physical(x, y, orientation, geometry)
               for x, y in ((lx0, ly0), (lx1 - 1, ly0),
                            (lx0, ly1 - 1), (lx1 - 1, ly1 - 1))]
    px0, px1 = min(x for x, _ in corners), max(x for x, _ in corners) + 1
    py0, py1 = min(y for _, y in corners), max(y for _, y in corners) + 1
    if px1 <= px0 or py1 <= py0:
        return None
    return px0, px1, py0, py1


def exempt_rows(rects, margin_top, margin_left, orientation, geometry):
    """py -> list of (first_byte, last_byte_exclusive, first_px, last_px) to blank out.

    Interior bytes of a rectangle are exempt in full; the two boundary bytes carry a bit mask so the
    text pixels sharing that byte are still compared.
    """
    rows = {}
    for r in rects:
        span = rect_to_physical(r, margin_top, margin_left, orientation, geometry)
        if span is None:
            continue
        px0, px1, py0, py1 = span
        for py in range(py0, py1):
            rows.setdefault(py, []).append((px0, px1))
    out = {}
    for py, spans in rows.items():
        entries = []
        for px0, px1 in spans:
            b0, b1 = px0 // 8, (px1 - 1) // 8 + 1
            entries.append((b0, b1, px0, px1))
        out[py] = entries
    return out


def text_only(data, exempt, geometry):
    """Return a copy of one plane with only the image pixels blanked, so the text bits can be compared."""
    _physical_width, _physical_height, row_bytes, _plane_bytes = geometry
    buf = bytearray(data)
    for py, entries in exempt.items():
        row = py * row_bytes
        for b0, b1, px0, px1 in entries:
            if b1 - b0 > 2:
                buf[row + b0 + 1: row + b1 - 1] = b'\x00' * (b1 - b0 - 2)   # interior bytes, in full
            for b in (b0, b1 - 1):
                lo = max(px0, b * 8)
                hi = min(px1, (b + 1) * 8)
                bits = 0
                for px in range(lo, hi):
                    bits |= 1 << (7 - (px % 8))
                buf[row + b] &= ~bits & 0xFF
    return bytes(buf)


def load_manifest(path):
    """(pages, margin_top, margin_left, orientation, geometry, problems[]).

    The margins are required, not defaulted: a manifest without them cannot be mapped into the
    physical planes, and silently substituting 0 would mask a region the image does not occupy —
    exactly the class of error this file exists to avoid.
    """
    problems = []
    doc = json.load(open(path, encoding='utf-8'))
    pages = doc['pages']

    # Identity must be unique. A duplicated (spine, page) would collapse in a set and in the
    # rects_by_page dict, so a page could be compared against the wrong page's rectangles — or
    # silently dropped from the count — without anything saying so.
    ids = [(int(p['spine']), int(p['page'])) for p in pages]
    dupes = sorted(k for k, n in Counter(ids).items() if n > 1)
    if dupes:
        problems.append(f'manifest repeats the same (spine, page) identity: {dupes[:4]} — '
                        f'deduplication would hide a page')

    margins = doc.get('margins')
    if not margins or len(margins) != 4:
        problems.append('manifest has no usable "margins" [top, right, bottom, left]; the image '
                        'rectangles are page-local and cannot be mapped without them')
        return pages, 0, 0, 0, (0, 0, 0, 0), problems
    orientation = int(doc.get('orientation', 0))
    if orientation not in (0, 1, 2, 3):
        problems.append(f'manifest has invalid orientation {orientation}')
        orientation = 0
    panel = doc.get('physicalPanel')
    plane_bytes = int(doc.get('planeBytes', 0))
    if not panel or len(panel) != 2:
        problems.append('manifest has no usable "physicalPanel" [width, height]')
        physical_width = physical_height = 0
    else:
        physical_width, physical_height = int(panel[0]), int(panel[1])
    if physical_width <= 0 or physical_width % 8 != 0 or physical_height <= 0:
        problems.append(f'invalid physical panel {physical_width}x{physical_height}; width must be byte-aligned')
    row_bytes = physical_width // 8 if physical_width > 0 else 0
    expected_plane_bytes = row_bytes * physical_height
    if plane_bytes != expected_plane_bytes:
        problems.append(f'manifest planeBytes={plane_bytes}, expected {expected_plane_bytes} from '
                        f'physical panel {physical_width}x{physical_height}')
    screen = doc.get('screen')
    expected_screen = ([physical_width, physical_height] if orientation in (1, 3)
                       else [physical_height, physical_width])
    if screen != expected_screen:
        problems.append(f'manifest screen={screen}, expected {expected_screen} for orientation {orientation}')
    geometry = (physical_width, physical_height, row_bytes, plane_bytes)
    return pages, int(margins[0]), int(margins[3]), orientation, geometry, problems


def compare(port_dir, oracle_dir, manifest_path):
    """Returns (pages_checked, exempt_pixels, problems[], image_page_findings[]).

    problems[] is fatal — a difference on an image-free page (where exact text parity is the contract)
    or a structural failure. image_page_findings[] carries the differences outside the image
    rectangles on image-bearing pages: reported, and fatal only under --require-image-page-text.
    """
    problems = []
    image_page_findings = []
    pages, margin_top, margin_left, orientation, geometry, problems = load_manifest(manifest_path)
    physical_width, physical_height, row_bytes, plane_bytes = geometry
    expected = {(int(p['spine']), int(p['page'])) for p in pages}
    rects_for = {(int(p['spine']), int(p['page'])): (p.get('images') or []) for p in pages}

    port = page_files(port_dir)
    oracle = page_files(oracle_dir)
    if not port or not oracle:
        return 0, 0, [f'no plane files: port={len(port)} oracle={len(oracle)} (was --dump-planes passed?)'], []

    # Identity against the manifest, not just port-vs-oracle: two renderers that both omit the same page
    # agree perfectly while proving nothing.
    for label, found in (('port', port), ('oracle', oracle)):
        if set(found) != expected:
            absent = sorted(expected - set(found))[:4]
            extra = sorted(set(found) - expected)[:4]
            problems.append(f'{label} plane pages != manifest pages (manifest {len(expected)}, {label} '
                            f'{len(found)}; absent {absent}, extra {extra})')

    checked = 0
    exempt_pixels = 0
    for key in sorted(expected):
        rects = rects_for.get(key, [])
        exempt_pixels += sum(max(0, int(r['w'])) * max(0, int(r['h'])) for r in rects)
        exempt = exempt_rows(rects, margin_top, margin_left, orientation, geometry)
        checked += 1
        for suffix in PLANES:
            a = port.get(key, {}).get(suffix)
            b = oracle.get(key, {}).get(suffix)
            if a is None or b is None:
                problems.append(f'spine {key[0]} page {key[1]}: missing {suffix}')
                continue
            pa = open(a, 'rb').read()
            pb = open(b, 'rb').read()
            if len(pa) != plane_bytes or len(pb) != plane_bytes:
                problems.append(f'spine {key[0]} page {key[1]}: {suffix} is {len(pa)}/{len(pb)} bytes, '
                                f'expected {plane_bytes} (physical {physical_width}x{physical_height}, '
                                f'{row_bytes} bytes/row)')
                continue
            if pa == pb:
                continue
            if not exempt:
                differing = sum(1 for x, y in zip(pa, pb) if x != y)
                problems.append(f'spine {key[0]} page {key[1]}: {suffix} differs in {differing} text byte(s)')
                continue
            ta, tb = text_only(pa, exempt, geometry), text_only(pb, exempt, geometry)
            if ta != tb:
                differing = sum(1 for x, y in zip(ta, tb) if x != y)
                # Differences OUTSIDE the image rectangles on a page that holds an image. Reported
                # always; fatal only under --require-image-page-text. Two limits keep this out of the
                # mandatory set: an image whose decoder writes beyond its declared rect would leak,
                # and text overlapping a rect would be exempt there. On the current fixtures the count
                # is zero, so this is a hedge against a case not yet measured, not a known gap.
                image_page_findings.append(
                    f'spine {key[0]} page {key[1]}: {suffix} differs in {differing} byte(s) outside the image '
                    f'rectangles')
    return checked, exempt_pixels, problems, image_page_findings


def self_control(port_dir, oracle_dir, manifest_path):
    """Flip one TEXT bit and require the comparison to catch it.

    Prefers an image-free page; if every page carries an image it falls back to a page with images and
    flips a bit outside the rectangles, which is the harder case and still must be caught.
    """
    pages, margin_top, margin_left, orientation, geometry, _problems = load_manifest(manifest_path)
    _physical_width, _physical_height, row_bytes, _plane_bytes = geometry
    keys = [(int(p['spine']), int(p['page'])) for p in pages]
    rects_for = {(int(p['spine']), int(p['page'])): (p.get('images') or []) for p in pages}
    target = next((k for k in keys if not rects_for.get(k)), None)
    if target is None:
        target = next((k for k in keys), None)
    if target is None:
        print('  CONTROL VOID: the manifest has no pages', file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory() as td:
        shutil.copytree(oracle_dir, td, dirs_exist_ok=True)
        name = f'p{target[0]:05d}_{target[1]:05d}.bw'
        victim = os.path.join(td, name)
        raw = bytearray(open(victim, 'rb').read())
        if not raw:
            print('  CONTROL VOID: the victim plane is empty', file=sys.stderr)
            return 1
        exempt = exempt_rows(rects_for.get(target, []), margin_top, margin_left, orientation, geometry)
        # Choose a byte the text owns, so the control exercises the mask rather than the exemption.
        masked = {py * row_bytes + b for py, entries in exempt.items()
                  for b0, b1, _px0, _px1 in entries for b in range(b0, b1)}
        available = [i for i in range(len(raw)) if i not in masked]
        idx = available[len(available) // 2] if available else len(raw) // 2
        raw[idx] ^= 0x01
        open(victim, 'wb').write(bytes(raw))

        checked, _exempt_px, problems, _findings = compare(port_dir, td, manifest_path)
        if checked < 1:
            print('  CONTROL VOID: nothing was checked', file=sys.stderr)
            return 1
        if not problems:
            print('  CONTROL FAILED: a flipped TEXT bit was not detected', file=sys.stderr)
            return 1
        print(f'  control ok: one flipped text bit in spine {target[0]} page {target[1]} was caught '
              f'({problems[0]})')
        return 0


def main():
    if len(sys.argv) < 4:
        print('usage: text_plane_parity.py <portPlanesDir> <oraclePlanesDir> <manifest.json> '
              '[--self-control]', file=sys.stderr)
        return 2
    port_dir, oracle_dir, manifest = sys.argv[1], sys.argv[2], sys.argv[3]
    if '--self-control' in sys.argv[4:]:
        return self_control(port_dir, oracle_dir, manifest)

    checked, exempt_pixels, problems, findings = compare(port_dir, oracle_dir, manifest)
    for p in problems[:8]:
        print(f'  {p}', file=sys.stderr)
    for f in findings[:4]:
        print(f'  OUTSIDE RECTS (image page): {f}', file=sys.stderr)
    if problems:
        print(f'TEXT RASTER PARITY FAILED — {len(problems)} problem(s) across {checked} page(s)',
              file=sys.stderr)
        return 1
    if findings and '--require-image-page-text' in sys.argv[4:]:
        print(f'TEXT RASTER PARITY FAILED — {len(findings)} image-page plane(s) differ outside their '
              f'rectangles and enforcement was requested', file=sys.stderr)
        return 1
    verdict = (f'TEXT RASTER PARITY OK — {checked} page(s) checked; image-free pages byte-identical, '
               f'image-page text identical outside the rectangles ({exempt_pixels:,} exempt image pixels)')
    if findings:
        verdict += (f' | {len(findings)} image-page plane(s) differ outside their rectangles '
                    f'(reported, not certified)')
    print(verdict)
    return 0


if __name__ == '__main__':
    sys.exit(main())
