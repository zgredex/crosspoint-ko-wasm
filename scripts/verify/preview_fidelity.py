#!/usr/bin/env python3
"""Does the preview tell the truth about the device?

The preview is a display transform over the page's four levels, so it can be scored exactly: a
page's average tone on the panel is the mean of its state reflectances, and what a viewer sees
on a monitor is the mean of those states' LUMINANCES (the eye integrates light, not display
codes - averaging sRGB bytes is the trap this script exists to avoid).

So, from the level histogram of an exported page:

    panel truth  = sum(freq(v) * R[v])                      R = 210/80/30/15
    preview      = 15 + 195 * sum(freq(v) * srgb_to_linear(P[v]))

A palette is faithful when those agree. Panel truth needs no source image: the four levels ARE
the panel's states, so the page's own level mix is the device's tone.

usage: preview_fidelity.py <file.xtch|file.xtc> [...]
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import container_diff  # noqa: E402  (index-driven container reader)

W, H = 480, 800
R = {0: 210.0, 1: 30.0, 2: 80.0, 3: 15.0}      # ink index v -> panel reflectance
R_BLACK, R_WHITE = 15.0, 210.0

# v = ink amount: 0 white, 1 dark grey, 2 light grey, 3 black
PALETTES = {
    'panel (now)': {0: 255, 1: 78, 2: 156, 3: 0},
    'bright (was)': {0: 255, 1: 128, 2: 205, 3: 0},
    'raw levels': {0: 255, 1: 170, 2: 85, 3: 0},   # the old 0/85/170/255 nominal palette
}


def srgb_to_linear(c):
    x = c / 255.0
    return x / 12.92 if x <= 0.04045 else ((x + 0.055) / 1.055) ** 2.4


def pages(path):
    """(magic, payload) pairs, read through the container's index.

    The payload shape is unchanged from the version this replaces (`d[q+22:q+rec]`). What changed is
    how the records are found: searching for the record magic and advancing 22 bytes past each hit
    lands on a false positive whenever a payload contains 'XTH\\0', and then skips the records after
    it. The container carries an index; use it.
    """
    records, _header = container_diff.container(path)
    out = []
    for rec in records:
        magic = rec[:4].rstrip(b'\x00').decode('ascii')
        out.append((magic, rec[22:]))
    return out


def histogram(magic, payload):
    """Level histogram over the page, in the writer's ink index v (0 white .. 3 black)."""
    hist = [0, 0, 0, 0]
    if magic == 'XTG':                      # one plane, logical row-major, bit 0 = ink
        for y in range(H):
            for bx in range(60):
                byte = payload[y * 60 + bx]
                for b in range(8):
                    x = bx * 8 + b
                    if x >= W:
                        break
                    ink = (byte >> (7 - b)) & 1
                    hist[0 if ink else 3] += 1
        return hist
    p1, p2 = payload[:48000], payload[48000:]   # physical columns, 100 B per column
    for phy in range(480):
        for c in range(100):
            b1, b2 = p1[phy * 100 + c], p2[phy * 100 + c]
            if b1 == 0 and b2 == 0:
                continue
            for b in range(8):
                y = c * 8 + b
                if y >= H:
                    break
                sh = 7 - b
                bit1 = (b1 >> sh) & 1
                bit2 = (b2 >> sh) & 1
                # (p1,p2) = (0,0) white, (0,1) dark grey, (1,0) light grey, (1,1) black
                v = 0 if (bit1 == 0 and bit2 == 0) else (1 if (bit1 == 0 and bit2 == 1) else
                                                         (2 if (bit1 == 1 and bit2 == 0) else 3))
                hist[v] += 1
    return hist


def main():
    files = sys.argv[1:]
    if not files:
        print(__doc__)
        return
    for path in files:
        try:
            pgs = pages(path)
        except OSError as e:
            print('%-34s %s' % (path.split('/')[-1], e))
            continue
        agg = [0, 0, 0, 0]
        for magic, payload in pgs:
            h = histogram(magic, payload)
            for i in range(4):
                agg[i] += h[i]
        n = sum(agg)
        if not n:
            print('%-34s no pages' % path.split('/')[-1])
            continue
        truth = sum(agg[v] * R[v] for v in range(4)) / n
        print('%-34s %s  pages %2d  levels white/dark/light/black = %5.1f%% %5.1f%% %5.1f%% %5.1f%%'
              % (path.split('/')[-1], pgs[0][0], len(pgs),
                 100 * agg[0] / n, 100 * agg[1] / n, 100 * agg[2] / n, 100 * agg[3] / n))
        print('    panel truth (device)  %6.1f' % truth)
        for name, pal in PALETTES.items():
            seen = sum(agg[v] * srgb_to_linear(pal[v]) for v in range(4)) / n
            tone = R_BLACK + (R_WHITE - R_BLACK) * seen
            print('    preview %-13s %6.1f   error %+6.1f reflectance units' % (name, tone, tone - truth))



if __name__ == '__main__':
    main()
