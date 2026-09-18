// Unit test for the 4-level image dither: constant-grey patches, no image content.
//
// Answers two questions with numbers:
//   1. does the 4-level quantiser dither (a hard quantiser gives ONE level per patch)?
//   2. is the pattern BLUE NOISE (aperiodic, anti-correlated at short range) rather
//      than a Bayer lattice (exactly periodic with period 4)?
//
// applyBayerDither4Level is the pre-change implementation kept in DitherUtils.h as the
// byte-level A/B anchor, so it is the natural positive control for "a lattice looks
// like THIS under the same metric".
//
// build: clang++ -O2 -I vendor-lib/Epub/Epub/converters dither_unit.cpp -o /tmp/ab/dither_unit
#include <cstdio>
#include <cmath>
#include <vector>

#include "DitherUtils.h"

static double autocorrAt(const std::vector<double>& f, int w, int h, int dx, int dy) {
  const double mean = [&] { double s = 0; for (double v : f) s += v; return s / f.size(); }();
  std::vector<double> r(f.size());
  double energy = 0;
  for (size_t i = 0; i < f.size(); i++) { r[i] = f[i] - mean; energy += r[i] * r[i]; }
  double s = 0;
  int n = 0;
  for (int y = 0; y + dy < h; y++)
    for (int x = 0; x + dx < w; x++) { s += r[y * w + x] * r[(y + dy) * w + x + dx]; n++; }
  return energy > 0 ? (s / n) / (energy / f.size()) : 0.0;
}

int main() {
  const int W = 64, H = 64;
  printf("4-level dither, constant-grey patches (%dx%d), profile=%s\n", W, H, kDefaultProfile.name);
  printf("levels        = {%u, %u, %u, %u}\n", kDefaultProfile.ditherLevels[0], kDefaultProfile.ditherLevels[1],
         kDefaultProfile.ditherLevels[2], kDefaultProfile.ditherLevels[3]);
  printf("thresholds    = {%u, %u, %u}  (used only when dithering is off)\n\n",
         kDefaultProfile.thresholds[0], kDefaultProfile.thresholds[1], kDefaultProfile.thresholds[2]);

  printf("%-8s | %-28s | %-28s\n", "grey", "blue noise (applyOrderedDither4Level)",
         "Bayer 4x4 (old anchor)");
  printf("%-8s | %-28s | %-28s\n", "", "mix%   mean  ac1   ac4", "mix%   mean  ac1   ac4");

  for (int grey : {20, 60, 110, 170, 220, 246}) {
    std::vector<double> bn, ba;
    double bnSum = 0, baSum = 0;
    int bnMix = 0, baMix = 0;
    for (int y = 0; y < H; y++) {
      for (int x = 0; x < W; x++) {
        const uint8_t a = applyOrderedDither4Level(static_cast<uint8_t>(grey), x, y);
        const uint8_t b = applyBayerDither4Level(static_cast<uint8_t>(grey), x, y);
        bn.push_back(a); ba.push_back(b);
        bnSum += a; baSum += b;
        bnMix += a; baMix += b;  // "mix" = mean level, i.e. the tone actually rendered
      }
    }
    const double bnMean = bnSum / bn.size(), baMean = baSum / ba.size();
    printf("grey %3d | %5.1f%% %6.2f %+.2f %+.2f      | %5.1f%% %6.2f %+.2f %+.2f\n", grey,
           100.0 * bnMix / (3.0 * bn.size()), bnMean, autocorrAt(bn, W, H, 1, 0),
           autocorrAt(bn, W, H, 4, 0), 100.0 * baMix / (3.0 * ba.size()), baMean,
           autocorrAt(ba, W, H, 1, 0), autocorrAt(ba, W, H, 4, 0));
  }

  // periodicity scan on one patch: a 4x4 lattice repeats at lag 4 in both axes
  std::vector<double> bn, ba;
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      bn.push_back(applyOrderedDither4Level(170, x, y));
      ba.push_back(applyBayerDither4Level(170, x, y));
    }
  printf("\nautocorrelation scan on grey 170 (1.0 = perfectly repeating at that lag)\n");
  printf("%-10s %10s %10s\n", "lag", "blue noise", "Bayer 4x4");
  for (int dx : {1, 2, 3, 4, 5, 8, 16, 32}) {
    printf("x +%-7d %10.3f %10.3f\n", dx, autocorrAt(bn, W, H, dx, 0), autocorrAt(ba, W, H, dx, 0));
  }
  for (int dy : {2, 4, 8}) {
    printf("y +%-7d %10.3f %10.3f\n", dy, autocorrAt(bn, W, H, 0, dy), autocorrAt(ba, W, H, 0, dy));
  }
  // with dithering off: one level per patch (the hard quantiser)
  printf("\ndithering OFF (quantizeToLevel) on the same greys:\n");
  for (int grey : {20, 60, 110, 170, 220, 246}) {
    printf("  grey %3d -> level %u\n", grey, quantizeToLevel(static_cast<uint8_t>(grey)));
  }
  return 0;
}
