#!/usr/bin/env python3
"""Measure whether 1-bit blue-noise dithering preserves the 4-level page's appearance.

Reference: the 2-bit XTH page (true 4-level grey, our "gold" rendering).
Candidates: the same page exported 1-bit with the dither ON and OFF.

Because dithering is by definition correct in the LOCAL AVERAGE and wrong per pixel,
the metric is block-mean luminance error: downsample both to 4x4 blocks and compare mean
reflectance against the reference. Lower = the mono page looks more like the real page.
A per-pixel comparison would be meaningless here (and would "favour" the hard threshold).
"""
import re
import struct
import sys

import numpy as np
from PIL import Image

# panel's four perceived states, indexed by the 2-bit value (0 white 1 dark 2 light 3 black)
LUM = np.array([210.0, 30.0, 80.0, 15.0], dtype=np.float32)
LW, LH = 480, 800


def pages(path, magic):
    d = open(path, 'rb').read()
    out, pos = [], 0
    while True:
        i = d.find(magic, pos)
        if i < 0:
            break
        out.append(i)
        pos = i + 22
    return d, out


def nth_page(d, offs, n, magic):
    """Payload after the magic: 18 more header bytes then the plane data."""
    assert 0 <= n < len(offs), f'page {n} out of range ({len(offs)} pages)'
    return d[offs[n] + 22: offs[n] + 22 + (96000 if magic == b'XTH\x00' else 48000)]


def decode_mono(payload):
    """XTG: row-major, 60 B/row, bit 0 = black."""
    img = np.zeros((LH, LW), dtype=np.float32)
    for y in range(LH):
        row = payload[y * 60:(y + 1) * 60]
        for bx in range(60):
            byte = row[bx]
            for bit in range(8):
                x = bx * 8 + bit
                img[y, x] = 15.0 if ((byte >> (7 - bit)) & 1) == 0 else 210.0
    return img


def decode_xth(payload):
    """XTH: two 48000 B planes; value = (plane1<<1)|plane2; phyX = y, phyY = 479 - x."""
    p1, p2 = payload[:48000], payload[48000:96000]
    img = np.zeros((LH, LW), dtype=np.float32)
    for y in range(LH):
        phyX = y
        byte_col = phyX >> 3
        bit = 7 - (phyX & 7)
        for x in range(LW):
            phyY = 479 - x
            i = phyY * 100 + byte_col
            v = (((p1[i] >> bit) & 1) << 1) | ((p2[i] >> bit) & 1)
            img[y, x] = LUM[v]
    return img


def block_mean(img, k=4):
    h, w = img.shape
    return img[:h // k * k, :w // k * k].reshape(h // k, k, w // k, k).mean(axis=(1, 3))


d2, o2 = pages('/tmp/ab/md_images.xtch', b'XTH\x00')
print(f'2-bit book: {len(o2)} pages')

# a text page from the text book instead - that is where AA matters
d2, o2 = pages('/tmp/ab/nc_demo.xtch', b'XTH\x00')
dm_on, om_on = pages('/tmp/ab/mono_on.xtc', b'XTG\x00')
dm_off, om_off = pages('/tmp/ab/mono_off.xtc', b'XTG\x00')
print(f'2-bit pages: {len(o2)} | mono pages: {len(om_on)} / {len(om_off)}')

for p in (20, 120, 500):
    ref = block_mean(decode_xth(nth_page(d2, o2, p, b'XTH\x00')))
    on = block_mean(decode_mono(nth_page(dm_on, om_on, p, b'XTG\x00')))
    off = block_mean(decode_mono(nth_page(dm_off, om_off, p, b'XTG\x00')))
    mae_on = float(np.abs(on - ref).mean())
    mae_off = float(np.abs(off - ref).mean())
    better = 'DITHER' if mae_on < mae_off else 'threshold'
    print(f'page {p:4d}: block-mean MAE vs 4-level -> dither {mae_on:6.2f} | threshold {mae_off:6.2f}'
          f'   ({better} wins by {abs(mae_off - mae_on):.2f})')

# visual A/B: a text band, x3
p = 120
ref = decode_xth(nth_page(d2, o2, p, b'XTH\x00'))
on = decode_mono(nth_page(dm_on, om_on, p, b'XTG\x00'))
off = decode_mono(nth_page(dm_off, om_off, p, b'XTG\x00'))
ink_rows = (off < 100).sum(axis=1)
y0 = int(np.argmax(np.convolve(ink_rows, np.ones(60), 'same'))) - 30
y0 = max(0, min(LH - 60, y0))
band = np.vstack([ref[y0:y0 + 60, 20:340], np.full((4, 320), 128.0),
                  off[y0:y0 + 60, 20:340], np.full((4, 320), 128.0),
                  on[y0:y0 + 60, 20:340]])
im = Image.fromarray(band.astype(np.uint8)).resize((320 * 3, band.shape[0] * 3), Image.NEAREST)
im.save('/tmp/ab/mono_ab.png')
print(f'\nA/B sheet /tmp/ab/mono_ab.png (page {p}, rows {y0}..{y0+60}, x3)')
print('  band 1 = 4-level reference (2-bit XTH), band 2 = 1-bit hard threshold, band 3 = 1-bit blue noise')
