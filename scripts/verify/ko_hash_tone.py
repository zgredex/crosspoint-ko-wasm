#!/usr/bin/env python3
"""What the "ko-fork hash" image dither is, and why it renders darker than the others.

Replicates the engine's decision rules verbatim - koForkHashDither (DitherUtils.h), the
bracketing in applyOrderedDither with the blue-noise table, and the 2-tone hash jitter from
ImageDither.h - then applies the panel's level reflectances to get the mean reflectance the
device would show for a flat patch of each source luma.

TWO TARGETS, and they answer different questions (mixing them up is the trap this file exists
to avoid):

  "tone"  - the source luma itself. A dithered patch is supposed to *reproduce* the tone by
            mixing the two bracketing panel levels, so this is the target for dither quality.
  "nearest state" - the panel step mapping (quantizeSimple bins 45/70/140). This is what
            "none" is defined to do; scoring a dither against it would say dithering is wrong.

Level convention (composer plane decode and kMonoInkDensity agree): the decoders return a
BRIGHTNESS index 0..3 packing as
    pv 0 -> black 15   pv 1 -> dark grey 30   pv 2 -> light grey 80   pv 3 -> white 210

usage: ko_hash_tone.py [table.h]
"""
import re
import sys

HEADER = sys.argv[1] if len(sys.argv) > 1 else 'vendor-lib/Epub/Epub/converters/BlueNoise64.h'
M32 = 0xFFFFFFFF
R = {0: 15.0, 1: 30.0, 2: 80.0, 3: 210.0}
LEVELS4 = (15.0, 30.0, 80.0, 210.0)      # firmware dither level luminances
LEVELS2 = (R[0], R[3])                   # 2-tone extremes: black 15, white 210
IDEAL_BINS = (45, 70, 140)               # quantizeSimple bins
MID2 = (LEVELS2[0] + LEVELS2[1]) / 2.0


def load_blue_noise(path=HEADER):
    txt = open(path).read()
    start = txt.index('{', txt.index('kBlueNoise64'))
    end = txt.index('}', start)
    vals = [int(v) for v in re.findall(r'\d+', txt[start + 1:end])]
    assert len(vals) == 64 * 64, 'expected 4096 values, parsed %d' % len(vals)
    return vals


BN = load_blue_noise()


def hash32(x, y):
    h = (x * 374761393 + y * 668265263) & M32
    return ((h ^ (h >> 13)) * 1274126177) & M32


def ko_fork_hash(gray, x, y):
    """koForkHashDither, verbatim: even thirds of the luma scale as bin edges (85, 170)."""
    thr = hash32(x, y) >> 24
    scaled = gray * 3
    if scaled < 255:
        return 1 if scaled + thr >= 255 else 0
    if scaled < 510:
        return 2 if (scaled - 255) + thr >= 255 else 1
    return 3 if (scaled - 510) + thr >= 255 else 2


def noise01(x, y):
    h = hash32(x, y)
    return ((h ^ (h >> 16)) & M32) / 4294967296.0


def blue_noise_ordered(v, x, y, levels=LEVELS4):
    """applyOrderedDither, BLUE_NOISE branch: bracket in `levels`, threshold the fraction."""
    lo = 0
    for k in range(len(levels) - 2, -1, -1):
        if v >= levels[k]:
            lo = k
            break
    hi = min(lo + 1, len(levels) - 1)
    span = levels[hi] - levels[lo]
    frac = 1.0 if lo == hi or span == 0 else (v - levels[lo]) / span
    t = (BN[((y & 63) << 6) | (x & 63)] + 0.5) / 256.0
    return hi if frac > t else lo


def nearest_state(gray):
    for i, b in enumerate(IDEAL_BINS):
        if gray < b:
            return i
    return 3


def ko_hash_two_tone(gray, x, y):
    """The fork's OWN quantize1bit: noise halved around 128, so solids stay solid."""
    threshold = int(noise01(x, y) * 256.0)
    return 3 if gray >= 128 + ((threshold - 128) // 2) else 0


def blue_noise_two_tone(gray, x, y):
    # ImageDither.h returns the BRIGHTNESS index (0 black / 3 white) for the 2-tone depth,
    # not the palette index, so map it here too before reading the reflectance table.
    return 3 if blue_noise_ordered(float(gray), x, y, LEVELS2) == 1 else 0


def patch_mean(fn, gray, size=64):
    return sum(R[fn(gray, x, y)] for y in range(size) for x in range(size)) / (size * size)


def errors(fn, two_tone=False):
    """mean/max |achieved - target| against the source tone, per depth."""
    es = []
    for g in range(256):
        m = patch_mean(fn, g)
        target = g if not two_tone else (R[0] if g < MID2 else R[3])
        es.append(abs(m - target))
    return sum(es) / 256, max(es)


def main():
    print('panel reflectances: black 15, dark grey 30, light grey 80, white 210')
    print('target for a dithered patch = the SOURCE TONE (mix the two bracketing states)\n')

    print('=== 4 tones (2-bit XTCH - the default preview mode) ===')
    print('  luma   tone   ko-hash  deviation   blue-noise  deviation')
    for gray in (0, 30, 60, 85, 100, 128, 160, 170, 200, 230, 255):
        kh = patch_mean(ko_fork_hash, gray)
        bn = patch_mean(lambda g, x, y: blue_noise_ordered(float(g), x, y), gray)
        flag = '  <- darker' if kh - gray < -20 else ''
        print('  %4d  %5.1f   %6.1f  %+8.1f     %6.1f  %+8.1f%s' %
              (gray, gray, kh, kh - gray, bn, bn - gray, flag))
    print()
    print('  model         4-tone mean|err|  max    |  2-tone mean|err|  max')
    for name, fn4, fn2 in (
        ('ko-hash', ko_fork_hash, ko_hash_two_tone),
        ('blue-noise', lambda g, x, y: blue_noise_ordered(float(g), x, y), blue_noise_two_tone),
        ('none (hard)', lambda g, x, y: nearest_state(g), ko_hash_two_tone),
    ):
        m4, x4 = errors(fn4)
        m2, x2 = (errors(fn2, two_tone=True) if name != 'none (hard)' else (None, None))
        print('  %-12s  %8.2f      %6.2f     |   %s' %
              (name, m4, x4, ('%8.2f      %6.2f' % (m2, x2)) if m2 is not None
               else '  (n/a - snaps to a state)'))

    print()
    print('=== 2 tones (1-bit XTC) ===')
    print('  luma   tone   ko-hash  blue-noise')
    for gray in (0, 60, 100, 128, 160, 200, 255):
        print('  %4d  %5.1f   %6.1f  %10.1f' % (
            gray, gray, patch_mean(ko_hash_two_tone, gray),
            patch_mean(blue_noise_two_tone, gray)))
    print()
    print('  what the two depth-2 rules do to SOLID black (luma 0), where 2 tones can only')
    print('  choose "ink" or "not ink":')
    print('    ko-hash (fork rule)   : %.2f%% of pure-black pixels turn white' %
          (100 * sum(1 for y in range(64) for x in range(64) if ko_hash_two_tone(0, x, y) == 3) / 4096))
    print('    blue-noise ordered    : %.2f%%' %
          (100 * sum(1 for y in range(64) for x in range(64)
                     if blue_noise_two_tone(0, x, y) == 3) / 4096))


main()
