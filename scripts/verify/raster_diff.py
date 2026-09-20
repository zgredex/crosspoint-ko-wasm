#!/usr/bin/env python3
"""Raster layer of the conformance contract: perceptual, not byte-identical.

The mechanical question ("are the BW/LSB/MSB planes byte-equal?") is the wrong gate for what
XTCKO is: the reference draws through its own rasterizer into an e-ink controller's plane
representation, and the browser draws through wasm. Byte equality is a *diagnostic* — worth
printing, worth keeping available, and not the standard. The standard is:

  a page fails the raster layer if a reader could see the difference as typography.
  It passes if the only differences are tone quantization.

Concretely, four measurements per page, in panel-level space (0..3) and in the project's
reflectance anchors (white 210, light 30, dark 80, black 15 — see plane_compare.py):

  differing pixels          how many pixels changed level at all
  max level delta           the worst single-pixel change, in levels
  moved                     the best integer shift in {-2..2}^2. If shifting one image makes
                            it match substantially better, the CONTENT MOVED — that is the
                            "glyph shifted 1 px / line moved" failure, and it is the one a
                            tolerance-based test must not absorb
  tone mass delta           box-blurred (radius 2) mean absolute reflectance difference. This
                            is what "visually equivalent" means numerically: it ignores a
                            reconfigured halftone pattern that carries the same mass

Verdict (only these fail a page):
  * moved                  best shift beats the unshifted match by > 8% of the differing
                           pixels AND leaves < 60% of the unshifted difference
  * max level delta >= 2   a two-level jump is at least a 50-unit reflectance change in these
                           anchors (210->30, 80->15, 210->80...), i.e. visible, not quantization
  * tone mass delta > 2.0  reflectance units of mean absolute change over a 5x5 box

Everything else is reported. `--strict-level1 F` additionally fails a page whose differing
pixel count exceeds a fraction of the page, which is how a caller can ask for a tighter bar
without returning to byte identity.

usage: raster_diff.py <reference> <candidate> [--limit N] [--strict-level1 FRACTION]
"""
import sys

sys.path.insert(0, __file__.rsplit('/', 1)[0])
import container_diff as cd  # noqa: E402

# ink level -> panel reflectance (project anchors)
REFLECT = {0: 210, 1: 80, 2: 30, 3: 15}

# Thresholds. Stated as constants because they are the contract, not tuning knobs.
MAX_LEVEL_DELTA = 2          # >= this fails: a visibly different tone
TONE_MASS_LIMIT = 2.0        # box-blurred mean |delta reflectance| that still reads as "same"
SHIFT_RANGE = 2
SHIFT_GAIN_RATIO = 0.08      # a shift must remove >8% of the difference to count as a move
SHIFT_RESIDUAL = 0.60        # ...and leave the rest under 60% of the unshifted difference


def decode_page(rec):
    """(levels, width, height); levels is 0..3 per portrait-record pixel."""
    width = int.from_bytes(rec[4:6], 'little')
    height = int.from_bytes(rec[6:8], 'little')
    plane_bytes = width * height // 8
    if rec[:4] == b'XTH\x00':
        p1 = rec[22:22 + plane_bytes]
        p2 = rec[22 + plane_bytes:22 + 2 * plane_bytes]
        column_bytes = height // 8
        lv = bytearray(width * height)
        for y in range(height):
            base = y * width
            for x in range(width):
                # physical column-major, columns right->left.
                off = (width - 1 - x) * column_bytes + (y >> 3)
                b1 = p1[off] >> (7 - (y & 7)) & 1
                b2 = p2[off] >> (7 - (y & 7)) & 1
                lv[base + x] = (b1 << 1) | b2
        return lv, width, height
    plane = rec[22:22 + plane_bytes]
    row_bytes = width // 8
    lv = bytearray(width * height)
    for y in range(height):
        base = y * width
        for x in range(width):
            # XTG: logical row-major, bit 0 = ink
            ink = (plane[y * row_bytes + (x >> 3)] >> (7 - (x & 7))) & 1
            lv[base + x] = 3 if ink == 0 else 0
    return lv, width, height


def box_blur(flat, width, height, rad=2):
    """Separable box blur (matches plane_compare.py's metric, without its O(r^2) inner loop)."""
    w, h = width, height
    tmp = [0.0] * (w * h)
    for y in range(h):
        row = y * w
        acc = 0.0
        for x in range(-rad, w + rad):
            if x < w:
                acc += flat[row + min(max(x, 0), w - 1)]
            if x - 2 * rad - 1 >= 0:
                acc -= flat[row + min(max(x - 2 * rad - 1, 0), w - 1)]
            if 0 <= x - rad < w:
                tmp[row + x - rad] = acc / (2 * rad + 1)
    out = [0.0] * (w * h)
    for x in range(w):
        acc = 0.0
        for y in range(-rad, h + rad):
            if y < h:
                acc += tmp[min(max(y, 0), h - 1) * w + x]
            if y - 2 * rad - 1 >= 0:
                acc -= tmp[min(max(y - 2 * rad - 1, 0), h - 1) * w + x]
            if 0 <= y - rad < h:
                out[(y - rad) * w + x] = acc / (2 * rad + 1)
    return out


def shifted_diff_count(a, b, width, height, dx, dy):
    """Pixels of `a` that differ from `b` when `b` is sampled at (x+dx, y+dy)."""
    n = 0
    for y in range(0, height, 2):   # every other row: the shift test only needs a robust signal
        ay = y * width
        by = min(max(y + dy, 0), height - 1) * width
        for x in range(0, width, 2):
            if a[ay + x] != b[by + min(max(x + dx, 0), width - 1)]:
                n += 1
    return n


def compare(a_levels, b_levels, width, height, label, strict_level1=None):
    n = width * height
    differing = 0
    max_delta = 0
    level_delta_hist = {}
    mass_a = [0.0] * n
    mass_b = [0.0] * n
    for i in range(n):
        la, lb = a_levels[i], b_levels[i]
        ra, rb = REFLECT[la], REFLECT[lb]
        mass_a[i] = ra
        mass_b[i] = rb
        if la != lb:
            differing += 1
            d = abs(la - lb)
            level_delta_hist[d] = level_delta_hist.get(d, 0) + 1
            if d > max_delta:
                max_delta = d
    tone = 0.0
    if differing:
        ba, bb = box_blur(mass_a, width, height), box_blur(mass_b, width, height)
        tone = sum(abs(x - y) for x, y in zip(ba, bb)) / n

    moved = None
    unshifted = shifted_diff_count(a_levels, b_levels, width, height, 0, 0)
    if differing and unshifted:
        best = (0, 0, unshifted)
        for dy in range(-SHIFT_RANGE, SHIFT_RANGE + 1):
            for dx in range(-SHIFT_RANGE, SHIFT_RANGE + 1):
                if dx == 0 and dy == 0:
                    continue
                c = shifted_diff_count(a_levels, b_levels, width, height, dx, dy)
                if c < best[2]:
                    best = (dx, dy, c)
        if best[2] < unshifted * (1 - SHIFT_GAIN_RATIO) and best[2] < unshifted * SHIFT_RESIDUAL:
            moved = best

    failures = []
    if moved:
        failures.append(f'content moved: shift {moved[0]:+d},{moved[1]:+d} removes '
                        f'{100.0 * (unshifted - moved[2]) / unshifted:.0f}% of the difference')
    if max_delta >= MAX_LEVEL_DELTA:
        failures.append(f'{level_delta_hist.get(max_delta, 0)} pixel(s) change {max_delta} levels '
                        f'(visible tone change, not quantization)')
    if tone > TONE_MASS_LIMIT:
        failures.append(f'tone mass delta {tone:.2f} > {TONE_MASS_LIMIT}')
    if strict_level1 is not None and differing > strict_level1 * n:
        failures.append(f'{differing} differing pixels > {100 * strict_level1:.2f}% of the page')

    print(f'  {label:24s} differ {differing:7d} ({100.0 * differing / n:5.2f}%)  '
          f'max {max_delta}  tone {tone:5.2f}  '
          f'levels {dict(sorted(level_delta_hist.items())) if level_delta_hist else "-"}  '
          f'{"MOVE " + str(moved) if moved else "no shift gain"}')
    return failures, {'differing': differing, 'max_level_delta': max_delta, 'tone_mass': tone,
                      'hist': level_delta_hist, 'moved': moved}, unshifted


def main():
    args, skip = [], False
    value_flags = ('--limit', '--sample', '--strict-level1', '--skip-images')
    for a in sys.argv[1:]:
        if skip:
            skip = False
            continue
        if a in value_flags:
            skip = True
            continue
        if a.startswith('--'):
            continue
        args.append(a)
    limit = int(sys.argv[sys.argv.index('--limit') + 1]) if '--limit' in sys.argv else 6
    strict = float(sys.argv[sys.argv.index('--strict-level1') + 1]) if '--strict-level1' in sys.argv else None
    sample = int(sys.argv[sys.argv.index('--sample') + 1]) if '--sample' in sys.argv else 0
    # Manifest-driven image-page skipping: image pixels differ by design (different decoders and
    # dither models), so a caller can exclude those pages from the verdict while still seeing
    # their numbers. --skip-images takes a manifest path.
    skip_images_manifest = (sys.argv[sys.argv.index('--skip-images') + 1]
                            if '--skip-images' in sys.argv else None)
    if len(args) != 2:
        print(__doc__)
        return 2
    ref, cand = cd.container(args[0])[0], cd.container(args[1])[0]
    if len(ref) != len(cand):
        print(f'page count differs: {len(ref)} vs {len(cand)}')
        return 1

    # The perceptual layer is ~2 s/page in Python; the EXACT layout layer covers every page, so
    # this one samples. An even spread, not the first N: a regression that only shows on the
    # last chapter must not slip through because the sample stopped at page 24.
    indices = list(range(len(ref)))
    if sample and sample < len(indices):
        step = len(indices) / sample
        indices = sorted({int(i * step) for i in range(sample)})
    image_pages = set()
    if skip_images_manifest:
        import json
        pages = json.load(open(skip_images_manifest, encoding='utf-8'))['pages']
        image_pages = {i for i, p in enumerate(pages) if p['images']}
    # Count what was actually compared, not what exists in the book: with sampling, the
    # whole-book image count says nothing about this run's verdict.
    excluded_in_sample = sorted(image_pages & set(indices))

    print(f'raster layer — {args[0].split("/")[-1]} vs {args[1].split("/")[-1]}: '
          f'{len(ref)} pages, comparing {len(indices)}'
          + (f' (sampled every {len(ref) / len(indices):.1f})' if len(indices) < len(ref) else '')
          + (f'; of those, {len(excluded_in_sample)} carry an image and are excluded from the '
             f'verdict ({len(image_pages)} image pages exist in the book)'
             if image_pages else ''))
    bad_pages, checked = [], 0
    # Totals are kept SEPARATE for judged and excluded pages. Mixing them produced a genuinely
    # misleading line on demo.epub — "differing pixels 151750, max level delta 3" printed next to
    # PASS, because the image pages' numbers were summed into the same counters. A reader has to
    # be able to see what the verdict was actually reached on.
    judged = {'differing': 0, 'max_level_delta': 0, 'pages': 0}
    excluded = {'differing': 0, 'max_level_delta': 0, 'pages': 0}
    worst = []
    for i in indices:
        ref_levels, ref_width, ref_height = decode_page(ref[i])
        cand_levels, cand_width, cand_height = decode_page(cand[i])
        if (ref_width, ref_height) != (cand_width, cand_height):
            bad_pages.append((i, [f'geometry differs: {ref_width}x{ref_height} vs '
                                  f'{cand_width}x{cand_height}']))
            continue
        failures, stats, _un = compare(ref_levels, cand_levels, ref_width, ref_height,
                                       f'page {i}', strict)
        bucket = excluded if i in image_pages else judged
        bucket['differing'] += stats['differing']
        bucket['max_level_delta'] = max(bucket['max_level_delta'], stats['max_level_delta'])
        bucket['pages'] += 1
        if i in image_pages:
            failures = []
        else:
            checked += 1
            if stats['differing']:
                worst.append((stats['tone_mass'], i))
        if failures:
            bad_pages.append((i, failures))
    worst.sort(reverse=True)
    if worst:
        print(f'  judged pages with any level difference: {len(worst)}; '
              f'largest tone-mass deltas: {[(i, round(m, 2)) for m, i in worst[:limit]]}')
    print(f'  judged:   {judged["pages"]} page(s), differing pixels {judged["differing"]}, '
          f'max level delta {judged["max_level_delta"]}')
    if excluded['pages']:
        print(f'  excluded: {excluded["pages"]} image page(s), differing pixels '
              f'{excluded["differing"]}, max level delta {excluded["max_level_delta"]} '
              f'— reported, not judged')
    if bad_pages:
        print(f'FAIL — {len(bad_pages)} page(s) fail the raster layer:')
        for i, f in bad_pages[:limit]:
            for line in f:
                print(f'    page {i}: {line}')
        return 1
    print('PASS — raster differences are tone quantization only: nothing moved, nothing '
          'changed by a visible tone step, tone mass within tolerance')
    return 0


if __name__ == '__main__':
    sys.exit(main())
