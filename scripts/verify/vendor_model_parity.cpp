// Parity gate: our dither decisions vs the OFFICIAL converter's model, over every grey.
//
// Vendor source (https://epub2xtc.xteink.cn/app.min.js?v=28, deobfuscated):
//   function quantize(v, t) {
//     return 1 === t ? (v < 128 ? 0 : 255)
//                    : (v > 212 ? 255 : v > 127 ? 170 : v > 42 ? 85 : 0);
//   }
//   function applyOutputQuantization(data, isHQ) {
//     ... i = isHQ ? (o > 212 ? 255 : o > 127 ? 170 : o > 42 ? 85 : 0) : (o >= 128 ? 255 : 0);
//   }
// and their dither is Floyd-Steinberg with the error scaled by a strength factor.
//
// What this gate can and cannot prove:
//   PROVES  the profile constants are the vendor's, the hard-threshold route is bit-identical
//           to their quantize() at both depths, and the 1-bit cut matches on every integer.
//   DOES NOT prove their default strength (75%) — we diffuse 100% of the error, so their
//           patches are smoother and slightly less exact. Documented, not claimed.
//
// build: clang++ -O2 -I vendor-lib/Epub/Epub/converters \
//          scripts/verify/vendor_model_parity.cpp -o /tmp/ab/vendor_parity
#include <cstdio>
#include <cstdint>

#include "DitherUtils.h"
#include "ImageDither.h"

static int vendorQuantize(int v, bool oneBit) {
  if (oneBit) return v < 128 ? 0 : 255;
  return v > 212 ? 255 : v > 127 ? 170 : v > 42 ? 85 : 0;
}

int main() {
  int fails = 0;

  // ---- 1. the profile itself must be the vendor's numbers -------------------
  const uint8_t wantT[3] = {42, 127, 212};
  const uint8_t wantL[4] = {0, 85, 170, 255};
  for (int i = 0; i < 3; ++i) {
    if (kDefaultProfile.thresholds[i] != wantT[i]) {
      printf("FAIL thresholds[%d] = %u, want %u\n", i, kDefaultProfile.thresholds[i], wantT[i]); ++fails;
    }
    if (kDefaultProfile.ditherThresholds[i] != wantT[i]) {
      printf("FAIL ditherThresholds[%d] = %u, want %u\n", i, kDefaultProfile.ditherThresholds[i], wantT[i]); ++fails;
    }
  }
  for (int i = 0; i < 4; ++i) {
    if (kDefaultProfile.ditherLevels[i] != wantL[i]) {
      printf("FAIL ditherLevels[%d] = %u, want %u\n", i, kDefaultProfile.ditherLevels[i], wantL[i]); ++fails;
    }
  }

  // ---- 2. hard-threshold route (dithering off) == their quantize() ----------
  for (int g = 0; g <= 255; ++g) {
    const int ours = kDefaultProfile.ditherLevels[
        quantizeWithThresholds(static_cast<uint8_t>(g), kDefaultProfile.thresholds)];
    const int theirs = vendorQuantize(g, false);
    if (ours != theirs) { printf("FAIL 4-level hard g=%d ours=%d vendor=%d\n", g, ours, theirs); ++fails; }
  }

  // ---- 3. 1-bit cut: their v<128 ? 0 : 255, on every integer ----------------
  const float mid = 0.5f * (kDefaultProfile.ditherLevels[0] + kDefaultProfile.ditherLevels[3]);
  for (int g = 0; g <= 255; ++g) {
    const int ours = (static_cast<float>(g) >= mid) ? kDefaultProfile.ditherLevels[3]
                                                    : kDefaultProfile.ditherLevels[0];
    const int theirs = vendorQuantize(g, true);
    if (ours != theirs) { printf("FAIL 1-bit cut g=%d ours=%d vendor=%d\n", g, ours, theirs); ++fails; }
  }

  // ---- 4. end-to-end through the real class, per pixel, both depths ---------
  // A synthetic image of every grey in sequence, rendered through ImageDitherer with the
  // dithering OFF, must reproduce the vendor's quantize() pixel for pixel.
  ko::ImageDitherOptions opts;
  const int W = 256;   // one pixel per grey, in one row

  for (int depth = 4; depth >= 2; depth -= 2) {
    opts.mode = DitherMode::NONE;
    opts.profile = &kDefaultProfile;
    opts.toneDepth = static_cast<uint8_t>(depth);
    ko::ImageDitherer d(opts);
    d.reset(W, 0, 0);
    for (int g = 0; g <= 255; ++g) {
      const uint8_t level = d(static_cast<uint8_t>(g), g, 0);
      const int ours = kDefaultProfile.ditherLevels[level & 3];
      const int theirs = vendorQuantize(g, depth == 2);
      if (ours != theirs) {
        printf("FAIL e2e depth=%d g=%d level=%u -> %d, vendor=%d\n", depth, g, level, ours, theirs);
        ++fails;
      }
    }
  }

  // ---- 5. the 2-tone palette must be the profile's extremes -----------------
  // (this is the bug this file was written after: a frozen {15,210} pair)
  if (kDefaultProfile.ditherLevels[0] != 0 || kDefaultProfile.ditherLevels[3] != 255) {
    printf("FAIL 2-tone palette is not {0,255}\n"); ++fails;
  }

  // ---- 6. tone preservation: the property that makes matching the vendor meaningful ---
  // Under the nominal model a dithered flat patch must average back to its own grey, because
  // the four levels are evenly spaced and the cuts sit at their midpoints. Under the fork's
  // perceived-luminance levels (15/30/80/210) the same patch lands DARKER in code space -
  // which is the whole disagreement this gate pins down. Tolerance 2.5 luma units.
  {
    const int kPatch = 64;
    const int greys[] = {32, 64, 96, 128, 160, 192, 224};
    for (int depth = 4; depth >= 2; depth -= 2) {
      for (unsigned gi = 0; gi < sizeof(greys) / sizeof(greys[0]); ++gi) {
        const int g = greys[gi];
        ko::ImageDitherOptions o;
        o.mode = DitherMode::FS;
        o.profile = &kDefaultProfile;
        o.toneDepth = static_cast<uint8_t>(depth);
        ko::ImageDitherer d(o);
        d.reset(kPatch, 0, 0);
        double sum = 0;
        for (int y = 0; y < kPatch; ++y)
          for (int x = 0; x < kPatch; ++x)
            sum += kDefaultProfile.ditherLevels[d(static_cast<uint8_t>(g), x, y) & 3];
        const double mean = sum / (kPatch * kPatch);
        printf("  tone: depth=%d grey=%3d -> mean %6.2f  (%+.2f)\n", depth, g, mean, mean - g);
        if (mean < g - 2.5 || mean > g + 2.5) {
          printf("FAIL tone depth=%d grey=%d -> mean %.2f (should track the source grey)\n",
                 depth, g, mean);
          ++fails;
        }
      }
    }
  }

  // ---- 7. per-model tone fidelity in the OFFICIAL space ---------------------
  // Mean |patch mean - source grey| over a grey sweep, both depths, every model. These are
  // the numbers the docs/tooltip quote; re-run this file after any dither change.
  {
    struct M { const char* name; DitherMode mode; };
    const M models[] = {{"none", DitherMode::NONE},          {"bayer", DitherMode::BAYER},
                        {"blue-noise", DitherMode::BLUE_NOISE}, {"fs", DitherMode::FS},
                        {"atk", DitherMode::ATK},            {"jjn", DitherMode::JJN},
                        {"stucki", DitherMode::STUCKI},      {"burkes", DitherMode::BURKES},
                        {"zhou-fang", DitherMode::ZHOU_FANG}, {"ko-hash", DitherMode::KO_HASH}};
    const int patch = 32;
    printf("\nper-model tone error (official space, mean |patch mean - grey| over 16..240):\n");
    for (int depth = 4; depth >= 2; depth -= 2) {
      printf("  depth %d:", depth);
      for (const M& m : models) {
        double err = 0; int n = 0;
        for (int g = 16; g <= 240; g += 8) {
          ko::ImageDitherOptions o;
          o.mode = m.mode; o.profile = &kDefaultProfile; o.toneDepth = static_cast<uint8_t>(depth);
          ko::ImageDitherer d(o); d.reset(patch, 0, 0);
          double sum = 0;
          for (int y = 0; y < patch; ++y)
            for (int x = 0; x < patch; ++x)
              sum += kDefaultProfile.ditherLevels[d(static_cast<uint8_t>(g), x, y) & 3];
          err += std::abs(sum / (patch * patch) - g); ++n;
        }
        printf("  %s %.2f", m.name, err / n);
      }
      printf("\n");
    }
  }

  printf(fails == 0 ? "PASS — official model matched on 512 decisions, 512 e2e pixels, 14 tone patches\n"
                    : "FAILED — %d mismatches\n", fails);
  return fails == 0 ? 0 : 1;
}
