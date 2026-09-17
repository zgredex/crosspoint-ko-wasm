#pragma once

#include <stdint.h>

#include <algorithm>
#include <vector>

#include "BlueNoise64.h"

// ============================================================================
// Image quantization to the panel's 4 shades.
//
// ALGORITHMS are ported from zgredex/crosspoint-pxc-converter
// (src/domain/dither.ts + src/domain/quantize.ts), keeping its structure:
//   - ordered modes (bayer, blue-noise) bracket the sample between two
//     `ditherLevels` entries and compare the position to a tile threshold;
//   - error-diffusion modes bin the sample with `ditherThresholds` and diffuse
//     against `ditherLevels` (the PERCEIVED panel luminances, which are much
//     darker than the nominal 0/85/170/255);
//   - with dithering off, `thresholds` is used directly.
//
// THRESHOLDS are the ko fork's. Its own quantizer
// (vendor-lib/GfxRenderer/BitmapHelpers.cpp:quantizeSimple, "fine-tuned to the
// X4 display") is gray < 45 → 0, < 70 → 1, < 140 → 2, else 3, i.e. exactly the
// `master` preset's `thresholds` triple. That triple is what the ko fork
// actually ships, so it is the default here.
//
// NOT PORTED: 'zhou-fang'. Its kernels are large interpolated tables and the
// reference implementation modulates thresholds with Math.random(), which is not
// reproducible; it would need a deterministic noise source first. Flagged rather
// than faked.
// ============================================================================

// ---------------------------------------------------------------------------
// Profiles
// ---------------------------------------------------------------------------
struct QuantProfile {
  uint8_t thresholds[3];        // hard quantize (dither disabled)
  uint8_t ditherThresholds[3];  // error-diffusion bin selection
  uint8_t ditherLevels[4];      // reconstruction values
  const char* name;
};

// pr1614: the firmware's older nominal profile.
inline constexpr QuantProfile kProfilePr1614 = {{42, 127, 212}, {42, 127, 212}, {0, 85, 170, 255}, "pr1614"};

// master: default in the converter. `thresholds` == the ko fork's shipped
// quantizeSimple triple (45/70/140); the dither tables are the X4-perception
// values the converter carries.
inline constexpr QuantProfile kProfileMaster = {{45, 70, 140}, {30, 50, 140}, {15, 30, 80, 210}, "master"};

// kofork: the ko fork's thresholds used on BOTH paths (dither binning included),
// with nominal level luminances. This is the strictest reading of "keep the ko
// fork's thresholds" — kept selectable so it can be measured against `master`.
inline constexpr QuantProfile kProfileKoFork = {{45, 70, 140}, {45, 70, 140}, {0, 85, 170, 255}, "kofork"};

// Default: the ko fork's own triple (45/70/140) on BOTH paths, with nominal level
// luminances. This is the strictest reading of "keep the ko fork's thresholds", and
// it also measured better than `master` in every comparison run (ramp tone error
// 0.031 vs 0.185; real-photo local tone error 1.36 vs 11.63) — `master`'s
// perceived-luminance levels (15/30/80/210) are carried here but were never
// explained, so they are not the default. One line to switch.
inline constexpr QuantProfile kDefaultProfile = kProfileKoFork;

// ---------------------------------------------------------------------------
// Enumeration
// ---------------------------------------------------------------------------
enum class DitherMode : uint8_t {
  NONE = 0,
  BAYER,        // ordered, 4x4
  BLUE_NOISE,   // ordered, 64x64 blue noise
  FS,           // Floyd-Steinberg
  ATK,          // Atkinson
  JJN,          // Jarvis-Judice-Ninke
  STUCKI,
  BURKES,
  KO_HASH,      // the ko fork's own hash-based noise dither (BitmapHelpers.cpp)
};

// ---------------------------------------------------------------------------
// Plain quantization
// ---------------------------------------------------------------------------
inline uint8_t quantizeWithThresholds(float v, const uint8_t t[3]) {
  if (v < static_cast<float>(t[0])) return 0;
  if (v < static_cast<float>(t[1])) return 1;
  if (v < static_cast<float>(t[2])) return 2;
  return 3;
}

// Quantization with dithering switched off: the profile's hard threshold triple.
// For the default profile this is the ko fork's own shipped triple (45/70/140).
inline uint8_t quantizeToLevel(uint8_t gray) {
  return quantizeWithThresholds(static_cast<float>(gray), kDefaultProfile.thresholds);
}

// ---------------------------------------------------------------------------
// Ordered dithering (bayer / blue-noise), verbatim from the converter
// ---------------------------------------------------------------------------
inline constexpr uint8_t kBayer4x4[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};

inline uint32_t orderedThresholdQ16(DitherMode mode, int x, int y) {
  if (mode == DitherMode::BLUE_NOISE) return blueNoiseThresholdQ16(x, y);
  const uint32_t v = kBayer4x4[(y & 3) * 4 + (x & 3)];
  return ((v * 2u + 1u) * 65536u) / 32u;  // (v + 0.5) / 16
}

inline uint8_t applyOrderedDither(float v, int x, int y, DitherMode mode,
                                  const QuantProfile& p = kDefaultProfile) {
  if (v < 0.0f) v = 0.0f;
  if (v > 255.0f) v = 255.0f;
  const uint8_t* levels = p.ditherLevels;
  int lo = 0;
  for (int k = 2; k >= 0; k--) {
    if (v >= static_cast<float>(levels[k])) {
      lo = k;
      break;
    }
  }
  const int hi = (lo + 1 < 4) ? lo + 1 : 3;
  const int span = static_cast<int>(levels[hi]) - static_cast<int>(levels[lo]);
  const float frac = (lo == hi) ? 1.0f : (v - static_cast<float>(levels[lo])) / static_cast<float>(span ? span : 1);
  const uint32_t t = orderedThresholdQ16(mode, x, y);
  const float tq = static_cast<float>(t) / 65536.0f;
  return static_cast<uint8_t>(frac > tq ? hi : lo);
}

// Engine entry point used by the image decoders: blue noise on the ko fork
// thresholds. (Was a 4x4 Bayer with a +/-40 offset that could not span its own
// quantization cell — see the performance notes for the measurements.)
inline uint8_t applyOrderedDither4Level(uint8_t gray, int x, int y) {
  return applyOrderedDither(static_cast<float>(gray), x, y, DitherMode::BLUE_NOISE, kDefaultProfile);
}

// ---------------------------------------------------------------------------
// Error diffusion
//
// Stateful and order-dependent (row-major with optional serpentine). Kernels are
// verbatim from the converter; the quantizer differs in that bin selection uses
// `ditherThresholds` while the diffused error uses `ditherLevels`.
// ---------------------------------------------------------------------------
struct ErrorDiffusionDither {
  bool serpentine = true;
  int width = 0;
  int firstRow = 0;
  int dir = 1;
  // Error carried into current / next / next+1 row, with a two-entry margin on
  // each side so right-to-left rows cannot index below zero.
  static constexpr int kBase = 2;
  std::vector<float> line0, line1, line2;

  void reset(int w, int y0 = 0) {
    width = w;
    firstRow = y0;
    dir = 1;
    const size_t n = static_cast<size_t>(w) + 2 * kBase;
    line0.assign(n, 0.0f);
    line1.assign(n, 0.0f);
    line2.assign(n, 0.0f);
  }

  void beginRow(int y) { dir = serpentine ? (((y - firstRow) & 1) ? -1 : 1) : 1; }

  uint8_t apply(uint8_t gray, int x, DitherMode mode, const QuantProfile& p = kDefaultProfile) {
    const int k = x + kBase;
    float v = static_cast<float>(gray) + line0[k];
    if (v < 0.0f) v = 0.0f;
    if (v > 255.0f) v = 255.0f;
    const uint8_t qv = quantizeWithThresholds(v, p.ditherThresholds);
    const float e = v - static_cast<float>(p.ditherLevels[qv]);
    const int w = width;

    switch (mode) {
      case DitherMode::ATK: {
        const float c = e / 8.0f;
        if (x + 1 < w) line0[k + 1] += c;
        if (x + 2 < w) line0[k + 2] += c;
        if (x > 0) line1[k - 1] += c;
        line1[k] += c;
        if (x + 1 < w) line1[k + 1] += c;
        line2[k] += c;
        break;
      }
      case DitherMode::JJN: {
        const float c = e / 48.0f;
        if (x + 1 < w) line0[k + 1] += 7 * c;
        if (x + 2 < w) line0[k + 2] += 5 * c;
        if (x > 1) line1[k - 2] += 3 * c;
        if (x > 0) line1[k - 1] += 5 * c;
        line1[k] += 7 * c;
        if (x + 1 < w) line1[k + 1] += 5 * c;
        if (x + 2 < w) line1[k + 2] += 3 * c;
        if (x > 1) line2[k - 2] += c;
        if (x > 0) line2[k - 1] += 3 * c;
        line2[k] += 5 * c;
        if (x + 1 < w) line2[k + 1] += 3 * c;
        if (x + 2 < w) line2[k + 2] += c;
        break;
      }
      case DitherMode::STUCKI: {
        const float c = e / 42.0f;
        if (x + 1 < w) line0[k + 1] += 8 * c;
        if (x + 2 < w) line0[k + 2] += 4 * c;
        if (x > 1) line1[k - 2] += 2 * c;
        if (x > 0) line1[k - 1] += 4 * c;
        line1[k] += 8 * c;
        if (x + 1 < w) line1[k + 1] += 4 * c;
        if (x + 2 < w) line1[k + 2] += 2 * c;
        if (x > 1) line2[k - 2] += c;
        if (x > 0) line2[k - 1] += 2 * c;
        line2[k] += 4 * c;
        if (x + 1 < w) line2[k + 1] += 2 * c;
        if (x + 2 < w) line2[k + 2] += c;
        break;
      }
      case DitherMode::BURKES: {
        const float c = e / 32.0f;
        if (x + 1 < w) line0[k + 1] += 8 * c;
        if (x + 2 < w) line0[k + 2] += 4 * c;
        if (x > 1) line1[k - 2] += 2 * c;
        if (x > 0) line1[k - 1] += 4 * c;
        line1[k] += 8 * c;
        if (x + 1 < w) line1[k + 1] += 4 * c;
        if (x + 2 < w) line1[k + 2] += 2 * c;
        break;
      }
      default: {  // FS
        if (x + 1 < w) line0[k + 1] += e * 7.0f / 16.0f;
        if (x > 0) line1[k - 1] += e * 3.0f / 16.0f;
        line1[k] += e * 5.0f / 16.0f;
        if (x + 1 < w) line1[k + 1] += e * 1.0f / 16.0f;
        break;
      }
    }
    return qv;
  }

  void endRow() {
    line0.swap(line1);
    line1.swap(line2);
    std::fill(line2.begin(), line2.end(), 0.0f);
  }
};

// ---------------------------------------------------------------------------
// The ko fork's own dither (BitmapHelpers.cpp:quantizeNoise), for comparison.
// Deterministic integer hash, no table, no lattice. USE_NOISE_DITHERING is false
// in the fork, so this ships disabled there; it is measured here.
// ---------------------------------------------------------------------------
inline uint8_t koForkHashDither(int gray, int x, int y) {
  uint32_t hash = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u;
  hash = (hash ^ (hash >> 13)) * 1274126177u;
  const int threshold = static_cast<int>(hash >> 24);
  const int scaled = gray * 3;
  if (scaled < 255) {
    return (scaled + threshold >= 255) ? 1 : 0;
  } else if (scaled < 510) {
    return ((scaled - 255) + threshold >= 255) ? 2 : 1;
  }
  return ((scaled - 510) + threshold >= 255) ? 3 : 2;
}

// ---------------------------------------------------------------------------
// Reference: the ORIGINAL pre-change implementation (4x4 Bayer, +/-40 offset
// against 64-wide thresholds). Kept only so byte-level A/B against the old
// build stays possible; not used by the engine.
// ---------------------------------------------------------------------------
inline uint8_t applyBayerDither4Level(uint8_t gray, int x, int y) {
  // Original firmware matrix, flattened row-major. Do NOT "tidy" this: it is the
  // byte-level A/B anchor for verifying that other changes are inert.
  constexpr uint8_t bayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
  const int dither = (static_cast<int>(bayer[(y & 3) * 4 + (x & 3)]) - 8) * 5;
  int adjusted = gray + dither;
  if (adjusted < 0) adjusted = 0;
  if (adjusted > 255) adjusted = 255;
  if (adjusted < 64) return 0;
  if (adjusted < 128) return 1;
  if (adjusted < 192) return 2;
  return 3;
}
