#!/usr/bin/env python3
"""Blue-noise dithering for 1-bit (XTG) page export, default ON.

Why this is better than the upstream attempt (crosspoint-reader PR #2179):

  * PR #2179 used fixed ink densities per 4-level value: 160/255 (62%) for dark grey and
    48/255 (19%) for light grey. Those numbers are arbitrary. A dithered patch should have
    the SAME average reflectance as the grey level it replaces, which is computable from
    the panel's own four perceived states (measured elsewhere in this tree as
    ~15/30/80/210 for black/dark grey/light grey/white):

        density(R) = (white - R) / (white - black) = (210 - R) / 195
        light grey  R=80  -> 0.667  -> 170/255
        dark grey   R=30  -> 0.923  -> 235/255

  * The BW-only path (used before this change) simply dropped every grey edge to a hard
    threshold, so 1-bit pages had no anti-aliasing at all.

The 2-bit XTH path is untouched: it already carries true 4-level glyph coverage.
"""
import shutil
import sys

P = '/Users/patryk/krxtc/ko-wasm/src/xtch_writer.h'
shutil.copy(P, '/tmp/ab/xtch_writer.pre_mono_dither.h')
s = open(P).read()


def rep(old, new, count=1):
    global s
    got = s.count(old)
    if got != count:
        print(f'ABORT ({got} != {count}): {old.strip().splitlines()[0][:80]!r}')
        sys.exit(1)
    s = s.replace(old, new)


# 1. the blue-noise table + the density derivation
rep('#include <cstdint>',
    '''#include <cstdint>

#include "../vendor-lib/Epub/Epub/converters/BlueNoise64.h"  // 64x64 void-and-cluster

// Ink densities for the 1-bit dither, indexed by the 4-level grey value
// (v = 0 white | 1 dark grey | 2 light grey | 3 black).
//
// A dithered patch must have the same AVERAGE reflectance as the grey level it stands in
// for. With the panel's four states perceived as white 210 / light grey 80 / dark grey 30
// / black 15, density = (210 - R) / (210 - 15):
//
//   v=0 white      R=210 -> 0.000 ->   0
//   v=1 dark grey  R= 30 -> 0.923 -> 235
//   v=2 light grey R= 80 -> 0.667 -> 170
//   v=3 black      R= 15 -> 1.000 -> 255
//
// The upstream attempt used 62% / 19% here, which renders dark grey far too light and
// throws away most of the contrast the panel's greys actually have.
//
// NOTE: the 15/30/80/210 anchors come from the panel model used by the converter, not from
// a calibrated measurement of our own unit. If a real test pattern says otherwise, these
// three numbers are the only thing that needs to change.
inline constexpr uint8_t kMonoInkDensity[4] = {0, 235, 170, 255};''')

# 2. the dither switch (default ON)
rep('  bool addPageFromPlanes(',
    '''  // Blue-noise dithering of grey text on 1-bit pages. ON by default: without it every
  // grey edge collapses to a hard threshold and 1-bit output has no anti-aliasing at all.
  void setMonoGrayDither(bool on) { monoGrayDither_ = on; }
  bool monoGrayDither() const { return monoGrayDither_; }

  bool addPageFromPlanes(''')
rep('  bool addPageFromPlanes(const std::vector<uint8_t>& bw, const std::vector<uint8_t>& lsb,\n                         const std::vector<uint8_t>& msb) {',
    '  bool addPageFromPlanes(const std::vector<uint8_t>& bw, const std::vector<uint8_t>& lsb,\n                         const std::vector<uint8_t>& msb) {')
rep('      return addMonoPage(bw, LOGICAL_W, LOGICAL_H);',
    '      return addMonoPage(bw, lsb, msb, LOGICAL_W, LOGICAL_H);')

# 3. the mono page itself
rep('''  bool addMonoPage(const std::vector<uint8_t>& bw, uint16_t LOGICAL_W, uint16_t LOGICAL_H) {
    std::vector<uint8_t> plane(48000, 0xFF);  // start white (1)

    auto physBit = [](const std::vector<uint8_t>& buf, int phyX, int phyY) -> int {
      return (buf[phyY * 100 + (phyX >> 3)] >> (7 - (phyX & 7))) & 1;
    };

    for (int y = 0; y < LOGICAL_H; y++) {
      for (int x = 0; x < LOGICAL_W; x++) {
        const int phyX = y;
        const int phyY = 479 - x;
        const int ink = physBit(bw, phyX, phyY) == 0;  // ink bit = 0 in engine BW
        if (ink) {
          // XTG bit 0 = black
          plane[y * 60 + (x >> 3)] &= static_cast<uint8_t>(~(1 << (7 - (x & 7))));
        }
      }
    }''',
    '''  bool addMonoPage(const std::vector<uint8_t>& bw, const std::vector<uint8_t>& lsb,
                   const std::vector<uint8_t>& msb, uint16_t LOGICAL_W, uint16_t LOGICAL_H) {
    std::vector<uint8_t> plane(48000, 0xFF);  // start white (1)
    const bool haveGray = lsb.size() >= 48000 && msb.size() >= 48000;
    const bool dither = monoGrayDither_ && haveGray;

    auto physBit = [](const std::vector<uint8_t>& buf, int phyX, int phyY) -> int {
      return (buf[phyY * 100 + (phyX >> 3)] >> (7 - (phyX & 7))) & 1;
    };

    for (int y = 0; y < LOGICAL_H; y++) {
      for (int x = 0; x < LOGICAL_W; x++) {
        const int phyX = y;
        const int phyY = 479 - x;
        const int ink = physBit(bw, phyX, phyY) == 0;  // ink bit = 0 in engine BW
        bool putInk = ink;
        if (dither && ink) {
          // Recover the 4-level grey value from the two planes, using the same encoding
          // as addGrayPage: p1 = ink & ~l (v & 2), p2 = ink & (l | ~m) (v & 1).
          const int l = physBit(lsb, phyX, phyY);
          const int m = physBit(msb, phyX, phyY);
          const uint8_t v = static_cast<uint8_t>(((l ? 0 : 1) << 1) | ((l || !m) ? 1 : 0));
          // Blue-noise threshold: the density IS the ink fraction, so a grey edge pixel
          // inks in proportion to how dark it is instead of collapsing to solid black.
          putInk = BLUE_NOISE_64[phyY & 63][phyX & 63] < kMonoInkDensity[v];
        }
        if (putInk) {
          // XTG bit 0 = black
          plane[y * 60 + (x >> 3)] &= static_cast<uint8_t>(~(1 << (7 - (x & 7))));
        }
      }
    }''')

# 4. the flag
rep('  bool addPageFromPlanes(const std::vector<uint8_t>& bw, const std::vector<uint8_t>& lsb,\n                         const std::vector<uint8_t>& msb) {',
    '  bool addPageFromPlanes(const std::vector<uint8_t>& bw, const std::vector<uint8_t>& lsb,\n                         const std::vector<uint8_t>& msb) {')
if 'bool monoGrayDither_ = true;' not in s:
    rep('  void setMonoGrayDither(bool on) { monoGrayDither_ = on; }',
        '  void setMonoGrayDither(bool on) { monoGrayDither_ = on; }')

open(P, 'w').write(s)
if 'bool monoGrayDither_ = true;' not in s:
    s = open(P).read()
    # add the member next to the setter's class scope: append before the closing of the class
    idx = s.rfind('};')
    s = s[:idx] + '  bool monoGrayDither_ = true;\n' + s[idx:]
    open(P, 'w').write(s)
print('mono blue-noise dither implemented, default ON')
print('  densities:', [l.strip() for l in s.split('\n') if 'kMonoInkDensity[4]' in l])
print('  flag present:', 'bool monoGrayDither_ = true;' in s)
