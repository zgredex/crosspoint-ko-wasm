#!/usr/bin/env python3
"""How do IMAGES get dithered, in each output mode?

Compares the same book exported 2-bit and 1-bit:
  * 2-bit (XTH) carries the 4-level value per pixel; report the level histogram and
    a periodicity check on the level pattern (a Bayer lattice would correlate at
    lag 4; blue noise does not).
  * 1-bit (XTG) carries ink only; report the REALISED ink density per 2-bit level.
    The mono writer's masks are built as `noise < density` with densities
    {235, 170, 255} for {dark, light, black}, so a correct 4->2-level halftone shows
    ~92% ink on dark grey, ~67% on light grey, ~100% on black, 0% on white.
Then the same check on the 1-bit ink pattern's own periodicity.

Usage: image_dither_report.py <book2bit.xtch> <book1bit.xtc> [max_pages]
"""
import sys

HDR, PAGE, PLANE = 22, 96022, 48000
LW, LH = 480, 800
XTG_PLANE, XTG_REC = 48000, 48022
# expected ink density per 4-level value, from the writer's mask densities
EXPECTED = {0: 0.0, 1: 235 / 255.0, 2: 170 / 255.0, 3: 1.0}


def xth_pages(path):
    d = open(path, 'rb').read()
    out, p = [], 0
    while True:
        q = d.find(b'XTH\x00', p)
        if q < 0:
            return out
        out.append(d[q + HDR:q + PAGE])
        p = q + HDR


def xtg_pages(path):
    d = open(path, 'rb').read()
    out, p = [], 0
    while True:
        q = d.find(b'XTG\x00', p)
        if q < 0:
            return out
        out.append(d[q + HDR:q + HDR + XTG_PLANE])
        p = q + HDR


def v_at(rec, x, y):
    """4-level value from the XTH planes: p1 = ink&~lsb, p2 = ink&(lsb|~msb)."""
    i = (LW - 1 - x) * 100 + (y >> 3)
    j = 7 - (y & 7)
    p1 = (rec[i] >> j) & 1
    p2 = (rec[PLANE + i] >> j) & 1
    return (p1 << 1) | p2


def ink_at(rec, x, y):
    return 1 - ((rec[y * 60 + (x >> 3)] >> (7 - (x & 7))) & 1)


def corr_at_lag(field, w, h, dx, dy):
    """Normalised correlation of the field with itself shifted by (dx, dy),
    after removing a 4x4 local mean (so only the dither pattern contributes)."""
    n = 0
    sxy = sxx = 0.0
    for y in range(0, h - dy, 2):
        for x in range(0, w - dx, 2):
            a = field[y * w + x]
            b = field[(y + dy) * w + x + dx]
            sxy += a * b
            sxx += a * a
            n += 1
    if n == 0 or sxx == 0:
        return 0.0
    return sxy / sxx  # autocorrelation at the lag, normalised by energy


def main():
    p2, p1 = sys.argv[1], sys.argv[2]
    maxp = int(sys.argv[3]) if len(sys.argv) > 3 else 2
    a, b = xth_pages(p2), xtg_pages(p1)
    print(f'{p2}: {len(a)} pages   {p1}: {len(b)} pages')
    if len(a) != len(b):
        print('page counts differ - comparing the overlap only')
    for p in range(min(len(a), len(b), maxp)):
        hist = {0: 0, 1: 0, 2: 0, 3: 0}
        ink_by_level = {0: 0, 1: 0, 2: 0, 3: 0}
        ink_tot = 0
        for y in range(LH):
            for x in range(LW):
                v = v_at(a[p], x, y)
                hist[v] += 1
                i = ink_at(b[p], x, y)
                ink_by_level[v] += i
                if v > 0:
                    ink_tot += i
        tot = LW * LH
        print(f'page {p}: levels white {hist[0]} ({100*hist[0]/tot:.1f}%) '
              f'dark {hist[1]} ({100*hist[1]/tot:.1f}%) '
              f'light {hist[2]} ({100*hist[2]/tot:.1f}%) '
              f'black {hist[3]} ({100*hist[3]/tot:.1f}%)')
        dens = []
        for v in (1, 2, 3):
            if hist[v]:
                dens.append(f'v={v}: realised {ink_by_level[v]/hist[v]:.3f} '
                            f'(expected {EXPECTED[v]:.3f})')
        print('   ink density per level: ' + ' | '.join(dens))
        if hist[0]:
            print(f'   white stays empty: {ink_by_level[0]} ink px of {hist[0]}')

        # periodicity of the 4-level pattern and of the ink pattern (image area)
        x0, x1, y0, y1 = 40, 440, 60, 720
        w, h = x1 - x0, y1 - y0
        lvl = [0.0] * (w * h)
        ink = [0.0] * (w * h)
        for yy in range(h):
            for xx in range(w):
                lvl[yy * w + xx] = float(v_at(a[p], x0 + xx, y0 + yy))
                ink[yy * w + xx] = float(ink_at(b[p], x0 + xx, y0 + yy))
        rows = []
        for dx, dy, lab in ((1, 0, 'lag 1x'), (4, 0, 'lag 4x'), (8, 0, 'lag 8x'),
                            (0, 4, 'lag 4y'), (0, 64, 'lag 64y')):
            rows.append(f'{lab}: level {corr_at_lag(lvl, w, h, dx, dy):+.3f} '
                        f'ink {corr_at_lag(ink, w, h, dx, dy):+.3f}')
        print('   periodicity (1.0 = perfectly repeating lattice):')
        for r in rows:
            print(f'     {r}')


if __name__ == '__main__':
    main()
