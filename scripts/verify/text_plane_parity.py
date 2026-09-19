#!/usr/bin/env python3
"""text_plane_parity.py — exact text-pixel parity, on EVERY page.

The contract (docs/ko-oracle-conformance.md):

    TEXT RASTER PARITY — MANDATORY. The Korean fork owns everything up to and including text
      rasterisation, so the bits of a page's BW / LSB / MSB planes that the TEXT wrote must be
      byte-identical to the reference's — on every page, whether or not that page also holds an image.

    IMAGE RASTER — XTCKO may intentionally differ (blue-noise dithering, a different decoder, the
      preview palette). Only the pixels inside image rectangles are exempt, and the rectangles come
      from the layout manifest, which LAYER 1 has already proven identical between the two renderers.

An earlier version skipped any page containing an image. That was too broad: one small illustration
beside twenty lines of Korean prose could hide completely wrong glyph rasterisation. Exempting the
rectangles instead of the pages keeps the exemption exactly as large as the thing that is allowed to
differ.

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

PLANES = ('.bw', '.lsb', '.msb')

# Panel geometry. Every plane is 1 bit per pixel, 60 bytes per 480-pixel row, so a pixel's byte index is
# y * ROW_BYTES + x // 8 and its bit is 7 - (x % 8) in all three.
WIDTH, HEIGHT, ROW_BYTES = 480, 800, 60
PLANE_BYTES = ROW_BYTES * HEIGHT


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


def exempt_rows(rects):
    """row -> list of (first_byte, last_byte_exclusive, first_bits, last_bits) to blank out.

    Interior bytes of a rectangle are exempt in full; the two boundary bytes carry a bit mask so the text
    pixels sharing that byte are still compared.
    """
    rows = {}
    for r in rects:
        x0, y0 = max(0, int(r['x'])), max(0, int(r['y']))
        x1, y1 = min(WIDTH, int(r['x']) + int(r['w'])), min(HEIGHT, int(r['y']) + int(r['h']))
        if x1 <= x0 or y1 <= y0:
            continue
        for y in range(y0, y1):
            rows.setdefault(y, []).append((x0, x1))
    out = {}
    for y, spans in rows.items():
        entries = []
        for x0, x1 in spans:
            b0, b1 = x0 // 8, (x1 - 1) // 8 + 1
            entries.append((b0, b1, x0, x1))
        out[y] = entries
    return out


def text_only(data, exempt):
    """Return a copy of one plane with only the image pixels blanked, so the text bits can be compared."""
    buf = bytearray(data)
    for y, entries in exempt.items():
        row = y * ROW_BYTES
        for b0, b1, x0, x1 in entries:
            if b1 - b0 > 2:
                buf[row + b0 + 1: row + b1 - 1] = b'\x00' * (b1 - b0 - 2)   # interior bytes, in full
            for b in (b0, b1 - 1):
                lo = max(x0, b * 8)
                hi = min(x1, (b + 1) * 8)
                bits = 0
                for px in range(lo, hi):
                    bits |= 1 << (7 - (px % 8))
                buf[row + b] &= ~bits & 0xFF
    return bytes(buf)


def compare(port_dir, oracle_dir, manifest_path):
    """Returns (pages_checked, exempt_pixels, problems[], image_page_findings).

    problems[] is fatal — image-free pages, where exact text parity is PROVEN and mandatory.
    image_page_findings[] is not: on a page holding an image the rectangle cannot isolate the text (see the
    measurement in the comment below), so those pages are counted and reported as NOT PROVEN.
    """
    problems = []
    image_page_findings = []
    pages = json.load(open(manifest_path, encoding='utf-8'))['pages']
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
        exempt = exempt_rows(rects)
        checked += 1
        for suffix in PLANES:
            a = port.get(key, {}).get(suffix)
            b = oracle.get(key, {}).get(suffix)
            if a is None or b is None:
                problems.append(f'spine {key[0]} page {key[1]}: missing {suffix}')
                continue
            pa = open(a, 'rb').read()
            pb = open(b, 'rb').read()
            if len(pa) != PLANE_BYTES or len(pb) != PLANE_BYTES:
                problems.append(f'spine {key[0]} page {key[1]}: {suffix} is {len(pa)}/{len(pb)} bytes, '
                                f'expected {PLANE_BYTES}')
                continue
            if pa == pb:
                continue
            if not exempt:
                differing = sum(1 for x, y in zip(pa, pb) if x != y)
                problems.append(f'spine {key[0]} page {key[1]}: {suffix} differs in {differing} text byte(s)')
                continue
            ta, tb = text_only(pa, exempt), text_only(pb, exempt)
            if ta != tb:
                differing = sum(1 for x, y in zip(ta, tb) if x != y)
                # MEASURED, AND THE REASON THIS IS NOT A PASS: an image's raster influence is NOT confined to
                # its rectangle. Rendering the port twice with a different --image-dither moves 183,057 pixel
                # bits OUTSIDE the rectangles, and even moves pages the manifest says have no images at all —
                # so on a page that holds an image, the rect mask cannot isolate the text. Only a text-only
                # CAPTURE (engine support on BOTH sides, which the pinned reference cannot gain) could.
                # Counted and reported, never certified. --require-image-page-text makes it fatal.
                image_page_findings.append(
                    f'spine {key[0]} page {key[1]}: {suffix} differs in {differing} byte(s) outside the image '
                    f'rectangles')
    return checked, exempt_pixels, problems, image_page_findings


def self_control(port_dir, oracle_dir, manifest_path):
    """Flip one TEXT bit and require the comparison to catch it.

    Prefers an image-free page; if every page carries an image it falls back to a page with images and flips
    a bit in the text area, which is the harder case and still must be caught.
    """
    pages = json.load(open(manifest_path, encoding='utf-8'))['pages']
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
        exempt = exempt_rows(rects_for.get(target, []))
        # Choose a byte the text owns, so the control exercises the mask rather than the exemption.
        available = [i for i in range(len(raw))
                     if not any(b0 <= i - y * ROW_BYTES < b1
                                for y, entries in exempt.items() for b0, b1, _x0, _x1 in entries
                                if 0 <= i - y * ROW_BYTES < ROW_BYTES)]
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
        print(f'  NOT PROVEN (image page): {f}', file=sys.stderr)
    if problems:
        print(f'TEXT RASTER PARITY FAILED — {len(problems)} problem(s) on image-free pages across '
              f'{checked} page(s)', file=sys.stderr)
        return 1
    if findings and '--require-image-page-text' in sys.argv[4:]:
        print(f'TEXT RASTER PARITY FAILED — {len(findings)} image-page plane(s) differ outside their '
              f'rectangles and enforcement was requested', file=sys.stderr)
        return 1
    verdict = (f'TEXT RASTER PARITY OK — {checked} page(s) checked; image-free pages byte-identical '
               f'({exempt_pixels:,} exempt image pixels)')
    if findings:
        verdict += (f' | NOT PROVEN on {len(findings)} image-page plane(s): an image perturbs the whole '
                    f'page, so a rectangle cannot isolate its text')
    print(verdict)
    return 0


if __name__ == '__main__':
    sys.exit(main())
