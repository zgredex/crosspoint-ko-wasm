#!/usr/bin/env python3
"""Isolate WHICH plane diverges: ink (BW), LSB (dark grey) or MSB (light grey).

Packed XTH value v = 0 if !ink, 1 if lsb, 2 if msb, 3 otherwise.
=> ink = v!=0 ; lsb = (v==1) ; msb = (v==1 or v==2)
"""
import sys

HDR, META, PAGE, PAGE_HDR = 56, 256, 96022, 22
PANEL_W, PANEL_H = 800, 480
BITS = PANEL_W * PANEL_H


def load(p):
    with open(p, 'rb') as f:
        return f.read()


def data_start(buf):
    for nch in range(0, 200):
        base = HDR + META + 96 * nch
        rem = len(buf) - base
        if rem <= 0:
            break
        if rem % (16 + PAGE) == 0:
            return base + 16 * (rem // (16 + PAGE))
    raise SystemExit('layout parse failed')


def levels(buf, p, dstart):
    off = dstart + p * PAGE + PAGE_HDR
    d = buf[off:off + 96000]
    out = bytearray(BITS)
    for i, b in enumerate(d):
        out[i * 4] = (b >> 6) & 3
        out[i * 4 + 1] = (b >> 4) & 3
        out[i * 4 + 2] = (b >> 2) & 3
        out[i * 4 + 3] = b & 3
    return out


def main():
    a, b = load(sys.argv[1]), load(sys.argv[2])
    da, db = data_start(a), data_start(b)
    npages = (len(a) - da) // PAGE
    tot = {'ink': 0, 'lsb': 0, 'msb': 0, 'pages': 0, 'px': 0}
    first = None
    for p in range(npages):
        va, vb = levels(a, p, da), levels(b, p, db)
        if va == vb:
            continue
        tot['pages'] += 1
        di = dl = dm = 0
        for i in range(BITS):
            x, y = va[i], vb[i]
            ink_a, ink_b = x != 0, y != 0
            if ink_a != ink_b:
                di += 1
            if (x == 1) != (y == 1):
                dl += 1
            if (x in (1, 2)) != (y in (1, 2)):
                dm += 1
        tot['ink'] += di
        tot['lsb'] += dl
        tot['msb'] += dm
        tot['px'] += sum(1 for i in range(BITS) if va[i] != vb[i])
        if first is None and (di or dl or dm):
            first = (p, di, dl, dm)
    print(f"pages: {npages}   differing pages: {tot['pages']}")
    print(f"differing pixels total: {tot['px']}")
    print(f"  INK (BW) mismatches : {tot['ink']}")
    print(f"  LSB plane mismatches: {tot['lsb']}")
    print(f"  MSB plane mismatches: {tot['msb']}")
    if first:
        print(f"first differing page {first[0]}: ink={first[1]} lsb={first[2]} msb={first[3]}")


if __name__ == '__main__':
    main()
