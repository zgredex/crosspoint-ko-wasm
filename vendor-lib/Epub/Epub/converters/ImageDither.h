#pragma once

#include <stdint.h>

#include "DitherUtils.h"

// ============================================================================
// Image dithering: which model, at which tone depth.
//
// The panel has two output depths and the model has to work at both:
//   2-bit XTCH -> 4 tones (levels[0..3] of the KO firmware's dither table)
//   1-bit XTC  -> 2 tones (the two EXTREMES of that same table: black and white)
// so the choice is not "4-level vs 1-level dithering" but ONE model applied at the depth
// the export needs.
//
// The model is the OFFICIAL converter's (epub2xtc.xteink.cn): bin selection 42/127/212 and
// reconstruction levels 0/85/170/255, which is also what srokl/xtcjsapp and
// x4converter.rho.sh ship. At 2 tones that gives the pair {0,255} and a cut at 127.5,
// i.e. the vendor's v<128 -> 0 : 255. kProfileMaster (the fork's X4-perception tables,
// 30/50/140 with 15/30/80/210) stays selectable for A/B measurement; the 1-bit writer's
// kMonoInkDensity 92%/67% masks are that model's emulation of its two greys and are used
// only for 1-bit TEXT anti-aliasing, not for image pixels.
//
// Models are from zgredex/crosspoint-pxc-converter (src/domain/dither.ts):
//   blue-noise, bayer (ordered), fs, atk, jjn, stucki, burkes (error diffusion),
//   zhou-fang (per-intensity kernels + threshold modulation), none (hard threshold),
//   plus the ko fork's own hash noise as a comparison point.
//
// COORDINATES. Ordered models keep using absolute screen coordinates, exactly as the
// pre-model engine did (blue noise is a 64x64 tile, so this fixes the tile phase to the
// page). The error-diffusion buffers are indexed by column, so those use image-local
// coordinates: an image placed at an inset origin would otherwise run off the end of the
// error rows. Serpentine parity stays on the absolute row so a tile's alignment does not
// depend on where the image sits.
// ============================================================================

namespace ko {

struct ImageDitherOptions {
  DitherMode mode = DitherMode::BLUE_NOISE;   // blue noise on the firmware's dither table
  uint8_t toneDepth = 4;                      // 4 = 2-bit page, 2 = 1-bit page
  const QuantProfile* profile = &kProfileNominal;   // official-model default
};

inline ImageDitherOptions& imageDitherOptionsRef() {
  static ImageDitherOptions opts;
  return opts;
}
inline void setImageDitherOptions(const ImageDitherOptions& o) { imageDitherOptionsRef() = o; }
inline const ImageDitherOptions& imageDitherOptions() { return imageDitherOptionsRef(); }

// The fork's own 1-bit noise dither, verbatim from BitmapHelpers.cpp:quantize1bit:
//   adjustedThreshold = 128 + (threshold - 128) / 2      (integer division -> range 64..192)
//   gray >= adjustedThreshold ? white : black
// `threshold` is the same hash byte as koForkHashDither. ditherNoise01() returns exactly
// hash/2^32, so threshold = ditherNoise01(x,y) * 256 is bit-exact (both are powers of two),
// which keeps this function free of a second copy of the hash.
inline uint8_t koForkHashDither1Bit(int gray, int x, int y) {
  const int threshold = static_cast<int>(ditherNoise01(x, y) * 256.0f);
  const int adjustedThreshold = 128 + ((threshold - 128) / 2);
  return (gray >= adjustedThreshold) ? 3 : 0;
}

inline bool isErrorDiffusionMode(DitherMode m) {
  switch (m) {
    case DitherMode::FS:
    case DitherMode::ATK:
    case DitherMode::JJN:
    case DitherMode::STUCKI:
    case DitherMode::BURKES:
    case DitherMode::ZHOU_FANG:
      return true;
    default:
      return false;
  }
}

// One instance per decoded image, called per pixel in scan order. Ordered models are
// stateless; the diffusion models keep their error rows here, so the decoders stay plain
// row-major loops and never manage dither state themselves.
class ImageDitherer {
 public:
  explicit ImageDitherer(const ImageDitherOptions& opt) : opt_(opt) {}

  // width/origin in screen pixels: the image occupies x in [originX, originX + width).
  void reset(int width, int originX, int originY) {
    width_ = width > 0 ? width : 1;
    originX_ = originX;
    originY_ = originY;
    ed_.reset(width_, originY);
    row_ = kNoRow;
  }

  uint8_t operator()(uint8_t gray, int x, int y) {
    const int ly = y - originY_;
    if (ly != row_) advanceRow(ly);
    const int lx = x - originX_;

    const uint8_t* levels = opt_.profile->ditherLevels;
    const uint8_t black = levels[0];
    const uint8_t white = levels[3];
    switch (opt_.mode) {
      case DitherMode::NONE:
        if (opt_.toneDepth >= 4) return quantizeWithThresholds(gray, opt_.profile->thresholds);
        return gray >= midTwoTone() ? 3 : 0;

      case DitherMode::KO_HASH:
        if (opt_.toneDepth >= 4) return koForkHashDither(gray, x, y);
        // 2 tones: the fork's OWN 1-bit rule (BitmapHelpers.cpp:quantize1bit), not an
        // invention of this port. Same hash, but the noise is halved around 128, so the
        // effective threshold spans 64..192: solids stay solid. A full-range +/-127 jitter
        // would turn 6.6% of pure-black pixels white, which the firmware never does.
        return koForkHashDither1Bit(gray, x, y);

      case DitherMode::ZHOU_FANG: {
        if (opt_.toneDepth >= 4) return ed_.applyZhouFang(gray, lx, ly, levels, 4);
        const uint8_t two[2] = {black, white};
        return ed_.applyZhouFang(gray, lx, ly, two, 2) ? 3 : 0;
      }

      case DitherMode::BLUE_NOISE:
      case DitherMode::BAYER:
        if (opt_.toneDepth >= 4) return applyOrderedDither(gray, x, y, opt_.mode, *opt_.profile);
        return orderedTwoTone(gray, x, y, black, white);

      default:  // FS / ATK / JJN / STUCKI / BURKES
        if (opt_.toneDepth >= 4) return ed_.apply(gray, lx, opt_.mode, *opt_.profile);
        return ed_.applyTwoTone(gray, lx, opt_.mode, black, white) ? 3 : 0;
    }
  }

 private:
  static constexpr int kNoRow = -1000000;

  float midTwoTone() const {
    const uint8_t* l = opt_.profile->ditherLevels;
    return 0.5f * (static_cast<float>(l[0]) + static_cast<float>(l[3]));
  }

  uint8_t orderedTwoTone(uint8_t gray, int x, int y, uint8_t black, uint8_t white) const {
    const float span = static_cast<float>(white) - static_cast<float>(black);
    const float frac = span > 0.0f ? (static_cast<float>(gray) - static_cast<float>(black)) / span : 1.0f;
    const float t = static_cast<float>(orderedThresholdQ16(opt_.mode, x, y)) / 65536.0f;
    return frac > t ? 3 : 0;
  }

  // Rows arrive in scan order; a rewind means a new pass (or a new image on the same
  // ditherer) and restarts the error state so output stays deterministic.
  void advanceRow(int ly) {
    if (row_ == kNoRow || ly < row_) {
      ed_.reset(width_, originY_);
      row_ = ly;
      return;
    }
    while (row_ < ly) {
      ed_.endRow();
      ++row_;
      ed_.beginRow(originY_ + row_);
    }
  }

  ImageDitherOptions opt_;
  ErrorDiffusionDither ed_;
  int width_ = 1;
  int originX_ = 0;
  int originY_ = 0;
  int row_ = kNoRow;
  // The 2-tone pair is NOT stored here: it is read from the active profile per pixel
  // (levels[0] = black, levels[3] = white). Freezing it to one profile is what made the
  // 1-bit route silently keep the fork's {15,210} pair after the model changed.
};

// Name -> model, for the host CLI and any tooling that should not hard-code the enum
// order. Unknown names return BLUE_NOISE (the default) rather than silently disabling
// dithering.
inline int ditherModeFromName(const char* name) {
  if (!name) return static_cast<int>(DitherMode::BLUE_NOISE);
  struct Entry { const char* n; DitherMode m; };
  static const Entry kEntries[] = {
      {"none", DitherMode::NONE},        {"bayer", DitherMode::BAYER},
      {"blue-noise", DitherMode::BLUE_NOISE}, {"bn", DitherMode::BLUE_NOISE},
      {"fs", DitherMode::FS},            {"floyd-steinberg", DitherMode::FS},
      {"atk", DitherMode::ATK},          {"atkinson", DitherMode::ATK},
      {"jjn", DitherMode::JJN},          {"jarvis", DitherMode::JJN},
      {"stucki", DitherMode::STUCKI},    {"burkes", DitherMode::BURKES},
      {"ko-hash", DitherMode::KO_HASH},  {"zhou-fang", DitherMode::ZHOU_FANG},
      {"zf", DitherMode::ZHOU_FANG},
  };
  for (const Entry& e : kEntries) {
    const char* a = e.n; const char* b = name;
    while (*a && *a == *b) { ++a; ++b; }
    if (*a == 0 && *b == 0) return static_cast<int>(e.m);
  }
  return static_cast<int>(DitherMode::BLUE_NOISE);
}

}  // namespace ko
