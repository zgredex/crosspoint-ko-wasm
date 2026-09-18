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
//     against `ditherLevels`;
//   - with dithering off, `thresholds` is used directly.
//
// THE MODEL IS THE OFFICIAL CONVERTER'S (epub2xtc.xteink.cn), because matching the vendor's
// reference pipeline is the point of this port. Its quantizer, verbatim from app.min.js:
//     quantize(v, t) = t === 1 ? (v < 128 ? 0 : 255)
//                              : (v > 212 ? 255 : v > 127 ? 170 : v > 42 ? 85 : 0)
// i.e. bin selection 42/127/212 with reconstruction levels 0/85/170/255, and both cuts
// INCLUSIVE at the lower bin (42 -> black, 127 -> 85, 212 -> 170). srokl/xtcjsapp and
// x4converter.rho.sh ship the identical triple. scripts/verify/vendor_model_parity.cpp
// proves our decisions equal that formula on every grey at both depths.
//
// The fork's own tables are kept as selectable alternates, not as the default:
//   kProfileMaster  - quantizeSimple 45/70/140 binning with perceived luminances
//                     15/30/80/210 (derived from the fork's 92%/67% 1-bit mask densities)
//   kProfileKoFork  - the same 45/70/140 triple on both paths with nominal levels
// ko-hash mode ignores the profile entirely: it is the fork's algorithm, verbatim.
//
// One deliberate difference from the vendor: their dither scales the diffused error by a
// strength factor (75% default for image regions, 50% for background) and their default
// mode dithers the WHOLE page, text included. We diffuse 100% of the error and dither
// images only (text keeps the firmwware's AA path). Both are documented, not hidden.
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

// nominal: the OFFICIAL converter's model, and what every reference implementation we
// could read ships. Verified verbatim in three places:
//   epub2xtc.xteink.cn (vendor)  : v>212?255 : v>127?170 : v>42?85 : 0   | 1-bit: v<128?0:255
//   srokl/xtcjsapp (assembly+ts) : same triple, same levels
//   x4converter.rho.sh           : same triple, same levels
// Levels are the evenly spaced 0/85/170/255 and the cuts are their midpoints. This is the
// vendor's own tool, so it is what we ship by default — see kDefaultProfile below.
// (crosspoint-pxc-converter labels this preset "PR1614", the firmware's older nominal set.)
inline constexpr QuantProfile kProfileNominal = {{42, 127, 212}, {42, 127, 212}, {0, 85, 170, 255}, "nominal"};

// master: the ko fork's X4-perception tables, as carried by crosspoint-pxc-converter.
// `thresholds` == the fork's shipped quantizeSimple triple (45/70/140); the dither tables
// assume the panel's four states perceive as ~15/30/80/210, derived from the fork's own
// 1-bit mask densities (92%/67% ink => reflectances ~30/~80 against a 15/210 panel).
// Kept selectable: it is the only model where ko-hash's dark mids are explained.
inline constexpr QuantProfile kProfileMaster = {{45, 70, 140}, {30, 50, 140}, {15, 30, 80, 210}, "master"};

// kofork: the ko fork's thresholds used on BOTH paths (dither binning included),
// with nominal level luminances. This is the strictest reading of "keep the ko
// fork's thresholds" — kept selectable so it can be measured against `master`.
inline constexpr QuantProfile kProfileKoFork = {{45, 70, 140}, {45, 70, 140}, {0, 85, 170, 255}, "kofork"};

// Default: the official converter's model. Our own reference for device truth is the
// vendor's tool, so when our reading of the fork's tables disagrees with it, the vendor
// wins — the point of this port is to produce what the official pipeline produces.
// `kProfileMaster` (fork perception) and `kProfileKoFork` (fork thresholds verbatim)
// remain selectable for A/B measurement. One line to switch.
inline constexpr QuantProfile kDefaultProfile = kProfileNominal;

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
  ZHOU_FANG,    // Zhou & Fang 2003: per-intensity kernels + threshold modulation
};

// ---------------------------------------------------------------------------
// Plain quantization
// ---------------------------------------------------------------------------
// The vendor's chain is `v > t2 ? 3 : v > t1 ? 2 : v > t0 ? 1 : 0`, so a value exactly ON a
// threshold stays in the LOWER bin (42 -> black). Hence `<=` here, not `<`: with `<` the
// three boundary greys landed one bin high and the parity gate caught it.
inline uint8_t quantizeWithThresholds(float v, const uint8_t t[3]) {
  if (v <= static_cast<float>(t[0])) return 0;
  if (v <= static_cast<float>(t[1])) return 1;
  if (v <= static_cast<float>(t[2])) return 2;
  return 3;
}

// Quantization with dithering switched off: the profile's hard threshold triple.
// For the default profile that is the official 42/127/212 -> 0/85/170/255.
inline uint8_t quantizeToLevel(uint8_t gray) {
  return quantizeWithThresholds(static_cast<float>(gray), kDefaultProfile.thresholds);
}

// ---------------------------------------------------------------------------
// Ordered dithering (bayer / blue-noise), verbatim from the converter
// ---------------------------------------------------------------------------
inline constexpr uint8_t kBayer4x4[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};

// ---------------------------------------------------------------------------
// Zhou & Fang, "Improving mid-tone quality of variable-coefficient error
// diffusion using threshold modulation", SIGGRAPH 2003 - tables ported verbatim
// from zgredex/crosspoint-pxc-converter (src/domain/dither.ts, ZF_MOD_KEYS /
// ZF_COEFF_KEYS), interpolated on the same keys and mirrored about 128.
//
// The reference draws its threshold modulation from Math.random(), which cannot be
// reproduced (and would make an export non-deterministic run to run), so the engine
// feeds the same formula a deterministic integer hash instead. Deliberate deviation:
// same distribution, reproducible output. See docs/image-dither-models.md.
// ---------------------------------------------------------------------------
inline float ditherNoise01(int x, int y) {
  uint32_t hash = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u;
  hash = (hash ^ (hash >> 13)) * 1274126177u;
  hash ^= hash >> 16;
  return static_cast<float>(hash >> 8) * (1.0f / 16777216.0f);   // [0,1)
}

struct ZhouFangTables {
  float mod[256];        // threshold modulation amplitude per source intensity
  float coeff[256 * 3];  // normalised (right, below-left, below) per source intensity
  ZhouFangTables() {
    static constexpr int kModN = 9;
    static constexpr int kModKey[kModN] = {0, 44, 64, 85, 95, 102, 107, 112, 127};
    static constexpr float kModVal[kModN] = {0.0f, 0.34f, 0.5f, 1.0f, 0.17f, 0.5f, 0.7f, 0.79f, 1.0f};
    static constexpr int kCoefN = 18;
    static constexpr int kCoefKey[kCoefN] = {0, 1, 2, 3, 4, 10, 22, 32, 44, 64, 72, 77, 85, 95, 102, 107, 112, 127};
    static constexpr int kCoefRaw[kCoefN][3] = {
        {13, 0, 5},          {1300249, 0, 499250},  {213113, 287, 99357}, {351854, 0, 199965},
        {801100, 0, 490999}, {704075, 297466, 303694}, {46613, 31917, 21469}, {47482, 30617, 21900},
        {43024, 42131, 14826}, {36411, 43219, 20369}, {38477, 53843, 7678}, {40503, 51547, 7948},
        {35865, 34108, 30026}, {34117, 36899, 28983}, {35464, 35049, 29485}, {16477, 18810, 14712},
        {33360, 37954, 28685}, {35269, 36066, 28664}};
    float halfMod[128] = {};
    for (int seg = 0; seg + 1 < kModN; seg++) {
      const int k0 = kModKey[seg], k1 = kModKey[seg + 1];
      const bool last = (seg == kModN - 2);
      const int num = k1 - k0 + (last ? 1 : 0);
      for (int j = 0; j < num; j++) {
        const float t = last ? static_cast<float>(j) / static_cast<float>(num - 1)
                             : static_cast<float>(j) / static_cast<float>(num);
        halfMod[k0 + j] = kModVal[seg] + t * (kModVal[seg + 1] - kModVal[seg]);
      }
    }
    float halfCoeff[3][128] = {};
    for (int seg = 0; seg + 1 < kCoefN; seg++) {
      const int k0 = kCoefKey[seg], k1 = kCoefKey[seg + 1];
      const bool last = (seg == kCoefN - 2);
      const int num = k1 - k0 + (last ? 1 : 0);
      for (int j = 0; j < num; j++) {
        const float t = last ? static_cast<float>(j) / static_cast<float>(num - 1)
                             : static_cast<float>(j) / static_cast<float>(num);
        for (int c = 0; c < 3; c++) {
          halfCoeff[c][k0 + j] = static_cast<float>(kCoefRaw[seg][c]) +
                                 t * static_cast<float>(kCoefRaw[seg + 1][c] - kCoefRaw[seg][c]);
        }
      }
    }
    for (int i = 0; i < 128; i++) {
      const float sum = halfCoeff[0][i] + halfCoeff[1][i] + halfCoeff[2][i];
      const float inv = sum > 0.0f ? 1.0f / sum : 0.0f;
      mod[i] = halfMod[i];
      mod[255 - i] = halfMod[i];
      for (int c = 0; c < 3; c++) {
        coeff[i * 3 + c] = halfCoeff[c][i] * inv;
        coeff[(255 - i) * 3 + c] = halfCoeff[c][i] * inv;
      }
    }
  }
};
inline const ZhouFangTables& zhouFangTables() {
  static const ZhouFangTables inst;
  return inst;
}

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

  // Scatters one pixel's error with the mode's kernel. Direction follows beginRow() so
  // serpentine scans mirror correctly: line0 = the row being written (ahead of x),
  // line1 = the next row, line2 = the row after that.
  void scatter(float e, int x, DitherMode mode) {
    const int k = x + kBase;
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
  }

  // Entry point for a 4-LEVEL page: bin with the profile's ditherThresholds, diffuse the
  // error between the sample and the profile's perceived level luminance.
  uint8_t apply(uint8_t gray, int x, DitherMode mode, const QuantProfile& p = kDefaultProfile) {
    const int k = x + kBase;
    float v = static_cast<float>(gray) + line0[k];
    if (v < 0.0f) v = 0.0f;
    if (v > 255.0f) v = 255.0f;
    const uint8_t qv = quantizeWithThresholds(v, p.ditherThresholds);
    scatter(v - static_cast<float>(p.ditherLevels[qv]), x, mode);
    return qv;
  }

  // Entry point for a 2-TONE page (1-bit XTC): the palette is the two extremes of the same
  // panel model, the binning threshold their midpoint. One pass, straight from the source -
  // no 4-level intermediate to dither a second time.
  uint8_t applyTwoTone(uint8_t gray, int x, DitherMode mode, uint8_t lo, uint8_t hi) {
    const int k = x + kBase;
    float v = static_cast<float>(gray) + line0[k];
    if (v < 0.0f) v = 0.0f;
    if (v > 255.0f) v = 255.0f;
    const float mid = 0.5f * (static_cast<float>(lo) + static_cast<float>(hi));
    const bool upper = v >= mid;
    scatter(v - static_cast<float>(upper ? hi : lo), x, mode);
    return upper ? 1 : 0;
  }

  // Zhou & Fang: thresholds are modulated by a per-intensity amplitude and the kernel is
  // per-intensity too, so it cannot share scatter(). levels[] must be ascending.
  uint8_t applyZhouFang(uint8_t gray, int x, int y, const uint8_t* levels, int nLevels) {
    const ZhouFangTables& zf = zhouFangTables();
    const int k = x + kBase;
    float v = static_cast<float>(gray) + line0[k];
    if (v < 0.0f) v = 0.0f;
    if (v > 255.0f) v = 255.0f;

    int lo = 0;
    for (int i = nLevels - 2; i >= 0; i--) {
      if (v >= static_cast<float>(levels[i])) {
        lo = i;
        break;
      }
    }
    const int hi = (lo + 1 < nLevels) ? lo + 1 : nLevels - 1;
    const float span = static_cast<float>(levels[hi]) - static_cast<float>(levels[lo]);
    const float frac = span > 0.0f ? (v - static_cast<float>(levels[lo])) / span : 1.0f;

    const int idx = v >= 255.0f ? 255 : static_cast<int>(v);
    const float r = ditherNoise01(x, y);
    const float thr = 0.5f + (r - static_cast<float>(static_cast<int>(r / 0.5f)) * 0.5f) * zf.mod[idx];
    const int qv = frac >= thr ? hi : lo;

    const float e = v - static_cast<float>(levels[qv]);
    const float* c = &zf.coeff[idx * 3];
    const int d = dir;
    if (x + d >= 0 && x + d < width) line0[k + d] += e * c[0];
    if (x - d >= 0 && x - d < width) line1[k - d] += e * c[1];
    line1[k] += e * c[2];
    return static_cast<uint8_t>(qv);
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
