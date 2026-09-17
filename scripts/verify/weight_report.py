#!/usr/bin/env python3
"""Quantify the 1-bit weight complaint.

Three pages of the same text book, exported three ways from the same source:
  2-bit XTH  - the reference (true 4-level grey)
  1-bit XTG  - blue-noise dither ON
  1-bit XTG  - blue-noise dither OFF (the old hard threshold)

Anchor-free numbers first (no dependence on the panel model), then the panel-model
numbers, then a calibration curve: expected ink mass for any candidate density table,
computed from the per-level pixel counts - so the decision can be made without rebuilding.
"""
import numpy as np

LH, LW = 800, 480
LUM = np.array([210.0, 30.0, 80.0, 15.0])   # 0 white 1 dark-grey 2 light-grey 3 black
WHITE, BLACK = 210.0, 15.0


def find_pages(path, magic, stride):
    d = open(path, 'rb').read()
    out, pos = [], 0
    while True:
        i = d.find(magic, pos)
        if i < 0:
            break
        out.append(i + 22)
        pos = i + 22
    return d, out


def unpack(plane, rows, rowbytes):
    return np.unpackbits(np.frombuffer(plane, dtype=np.uint8)[:rows * rowbytes]
                         .reshape(rows, rowbytes), axis=1)


def decode_xth(payload):
    p1, p2 = payload[:48000], payload[48000:96000]
    Y = np.arange(LH)[:, None]
    X = np.arange(LW)[None, :]
    phyX = np.broadcast_to(Y, (LH, LW))
    phyY = 479 - X
    idx = phyY * 100 + (phyX >> 3)
    bit = 7 - (phyX & 7)
    b1 = (np.frombuffer(p1, np.uint8)[idx] >> bit) & 1
    b2 = (np.frombuffer(p2, np.uint8)[idx] >> bit) & 1
    return ((b1 << 1) | b2).astype(np.uint8)


def decode_xg(payload):
    """XTG: bit 0 = black -> ink where bit == 0."""
    return (unpack(payload, LH, 60)[:, :LW] == 0)


d2, o2 = find_pages('/tmp/ab/nc_demo.xtch', b'XTH\x00', 96000)
don, oon = find_pages('/tmp/ab/mono_on.xtc', b'XTG\x00', 48000)
doff, ooff = find_pages('/tmp/ab/mono_off.xtc', b'XTG\x00', 48000)
print(f'pages: 2-bit {len(o2)} | mono dither {len(oon)} | mono threshold {len(ooff)}\n')

CAND = [
    ('current (235/170)', (0, 235, 170, 255)),
    ('x0.80 (188/136)', (0, 188, 136, 255)),
    ('x0.65 (153/111)', (0, 153, 111, 255)),
    ('x0.50 (118/ 85)', (0, 118, 85, 255)),
    ('PR#2179 (160/48)', (0, 160, 48, 255)),
]

print('page | v=0  v=1  v=2  v=3  (% of page)      | ink%% 2bit-implied  mono-dith  mono-thr | mono/core')
print('-' * 104)
for p in (20, 120, 500, 1000):
    v = decode_xth(d2[o2[p]:o2[p] + 96000] if False else
                   open('/tmp/ab/nc_demo.xtch', 'rb').read()[o2[p] - 22 + 22:o2[p] - 22 + 22 + 96000])
    ink_on = decode_xg(open('/tmp/ab/mono_on.xtc', 'rb').read()[oon[p]:oon[p] + 48000])
    ink_off = decode_xg(open('/tmp/ab/mono_off.xtc', 'rb').read()[ooff[p]:ooff[p] + 48000])
    n = v.size
    c = [int((v == k).sum()) for k in range(4)]
    implied = float(((WHITE - LUM[v]) / (WHITE - BLACK)).mean()) * 100
    on = ink_on.mean() * 100
    off = ink_off.mean() * 100
    core = c[3] / n * 100
    print(f'{p:4d} | {c[0]/n*100:5.1f} {c[1]/n*100:5.2f} {c[2]/n*100:5.2f} {c[3]/n*100:5.1f} '
          f'              | {implied:9.2f}      {on:8.2f}  {off:7.2f} | {on/max(core,1e-9):8.2f}x')

# per-level empirical density: is the implementation doing what it claims?
v = decode_xth(open('/tmp/ab/nc_demo.xtch', 'rb').read()[o2[120] - 22 + 22:o2[120] - 22 + 22 + 96000])
ink_on = decode_xg(open('/tmp/ab/mono_on.xtc', 'rb').read()[oon[120]:oon[120] + 48000])
ink_off = decode_xg(open('/tmp/ab/mono_off.xtc', 'rb').read()[ooff[120]:ooff[120] + 48000])
print('\nempirical density actually applied, by grey level (page 120):')
print('  level      pixels   mono-dither   mono-threshold   nominal')
nominal = {1: 235 / 255, 2: 170 / 255, 3: 1.0}
for k in (1, 2, 3):
    m = v == k
    if m.sum() == 0:
        continue
    print(f'  v={k}   {m.sum():9d}      {ink_on[m].mean():6.3f}         {ink_off[m].mean():6.3f}     {nominal[k]:6.3f}')

print('\ncalibration curve - expected ink mass for candidate densities (page 120):')
n = v.size
c = {k: int((v == k).sum()) for k in range(4)}
implied = float(((WHITE - LUM[v]) / (WHITE - BLACK)).mean()) * 100
print(f'  2-bit reference, implied ink mass              : {implied:6.2f}%')
print(f'  1-bit hard threshold (what shipped before)     : {100*(c[1]+c[2]+c[3])/n:6.2f}%')
for name, tab in CAND:
    mass = 100 * (c[1] * tab[1] / 255 + c[2] * tab[2] / 255 + c[3] * tab[3] / 255) / n
    print(f'  {name:30s}                : {mass:6.2f}%')
