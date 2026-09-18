#!/usr/bin/env python3
"""Compare exported pages in PANEL REFLECTANCE space, straight from the container planes.

Why from the files and not the engine: this is the artifact the device shows, and it lets an
old build and a new build be compared without either one having to run the other's code.

Plane layouts (they differ, and getting this wrong silently compares unrelated bits):
  XTG (1-bit): logical row-major - byte y*60 + (x>>3), bit 7-(x&7), bit 0 = ink.
  XTH (2-bit): PHYSICAL column-major - for logical (x,y): byte (479-x)*100 + (y>>3),
               bit 7-(y&7); two planes p1,p2 which decode to the 4 levels as
               (p1,p2) = (0,0) white, (0,1) dark grey, (1,0) light grey, (1,1) black.
Panel model (the project's own anchors): white 210, light grey 80, dark grey 30, black 15.

usage: plane_compare.py <ref.xtch> <cand.xtg> [<cand2.xtg> ...] [--pages N]
"""
import sys

import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import container_diff  # noqa: E402  (index-driven container reader, shared with the other tools)

W, H = 480, 800
R = {0: 210, 1: 80, 2: 30, 3: 15}     # level -> reflectance (v = ink amount, 3 = black)
R_WHITE, R_BLACK = 210, 15
RANGE = R_WHITE - R_BLACK


def pages(path):
    """Page records including their 22-byte record headers, read through the container's index.

    The returned shape is unchanged from the version this replaces (`d[q:q+rec]` also spanned the
    header). What changed is how the records are found: that version searched for the record magic
    and advanced 22 bytes past each hit, which lands on a false positive whenever a payload contains
    'XTH\\0' and then skips the records that follow it. The container carries an index; use it, and
    let the reader throw rather than return fewer records than the writer wrote.
    """
    records, _header = container_diff.container(path)
    return list(records)


def bit(plane, x, y):
    return (plane[(479 - x) * 100 + (y >> 3)] >> (7 - (y & 7))) & 1


def bit_logical(plane, x, y):
    return (plane[y * 60 + (x >> 3)] >> (7 - (x & 7))) & 1


def decode_xth(rec):
    p1, p2 = rec[22:22 + 48000], rec[22 + 48000:22 + 96000]
    out = bytearray(W * H)
    for y in range(H):
        base = y * W
        for x in range(W):
            out[base + x] = (bit(p1, x, y) << 1) | bit(p2, x, y)   # 0 white .. 3 black
    return out


def decode_xtg(rec):
    """Return reflectance per pixel (210 white / 15 ink)."""
    plane = rec[22:22 + 48000]
    out = bytearray(W * H)
    for y in range(H):
        base = y * W
        for x in range(W):
            out[base + x] = R_BLACK if bit_logical(plane, x, y) == 0 else R_WHITE
    return out


def box(flat, rad=2):
    out = [0.0] * (W * H)
    for y in range(H):
        y0, y1 = max(0, y - rad), min(H - 1, y + rad)
        for x in range(W):
            x0, x1 = max(0, x - rad), min(W - 1, x + rad)
            s, n = 0.0, 0
            for yy in range(y0, y1 + 1):
                row = yy * W
                for xx in range(x0, x1 + 1):
                    s += flat[row + xx]
                    n += 1
            out[y * W + x] = s / n
    return out


def stats(ref_levels, cand_refl, label):
    ref_refl = [R[v] for v in ref_levels]
    br, bc = box(ref_refl), box(cand_refl)
    tone = sum(abs(a - b) for a, b in zip(br, bc)) / (W * H)
    # per-level tone error, to say WHICH level is being misrepresented
    per = {}
    for i, v in enumerate(ref_levels):
        e = abs(br[i] - bc[i])
        cur = per.setdefault(v, [0.0, 0])
        cur[0] += e
        cur[1] += 1
    pepper = 0
    ink = sum(1 for v in cand_refl if v == R_BLACK)
    for y in range(1, H - 1):
        for x in range(1, W - 1):
            i = y * W + x
            dark = cand_refl[i] == R_BLACK
            same = lambda j: (cand_refl[j] == R_BLACK) == dark
            if not same(i - 1) and not same(i + 1) and not same(i - W) and not same(i + W):
                pepper += 1
    print('    %-22s tone %5.2f%%  ink %5.2f%%  pepper/1k %5.1f   per-level: %s' % (
        label, 100 * tone / RANGE, 100 * ink / (W * H), 1000 * pepper / (W * H),
        '  '.join('%s %5.2f%%' % ({0: 'white', 1: 'dark-grey', 2: 'light-grey', 3: 'black'}[v],
                                  100 * per[v][0] / per[v][1] / RANGE)
                  for v in sorted(per, key=lambda k: -per[k][1]) if per[v][1] > 200)))


def main():
    skip = set()
    for opt in ('--pages', '--list'):
        if opt in sys.argv:
            skip.add(sys.argv.index(opt) + 1)
    args = [a for i, a in enumerate(sys.argv[1:], start=1) if not a.startswith('--') and i not in skip]
    n_pages = int(sys.argv[sys.argv.index('--pages') + 1]) if '--pages' in sys.argv else 5
    only = [int(v) for v in sys.argv[sys.argv.index('--list') + 1].split(',')] if '--list' in sys.argv else None
    ref, cands = args[0], args[1:]
    refp = pages(ref)
    print('reference (4-level): %s  %d pages' % (ref.split('/')[-1], len(refp)))
    for c in cands:
        cp = pages(c)
        print('  candidate: %s  %d pages' % (c.split('/')[-1], len(cp)))
        agg = {}
        idxs = only if only else list(range(min(n_pages, len(refp), len(cp))))
        for i in idxs:
            lv = decode_xth(refp[i])
            cr = decode_xtg(cp[i])
            print('  page %d:' % i)
            stats(lv, cr, c.split('/')[-1][:22])
        break   # one candidate at a time keeps the output readable



if __name__ == '__main__':
    main()
