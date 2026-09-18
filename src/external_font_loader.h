// Parser for the lossless external built-in font blob ("EPD2").
//
// Deliberately dependency-light: this parses bytes into runtime structures and nothing else. Wiring the
// result into EpdFont/EpdFontFamily/renderer happens where the renderer lives (wasm_api.cpp), and the
// identical parser is exercised by tools/verify_external_font.cpp against the embedded original — one
// implementation, so "the loader is lossless" is a statement about the code that actually ships.
//
// Every byte of the source arrays is copied through, including 12.4 fixed-point advances and the kern
// matrix; nothing is quantised, rescaled or dropped. Parse failures are explicit and never partial:
// `out` is only populated on success.
#pragma once
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "external_font_blob.h"

namespace ko {

// One owned external built-in font. The vectors own the storage; `data` points into them, so this
// struct must not be copied (the pointers would dangle) — it is heap-allocated and moved by pointer.
struct ExternalBuiltinFont {
  std::vector<EpdUnicodeInterval> intervals;
  std::vector<EpdGlyph> glyphs;
  std::vector<uint8_t> bitmap;
  std::vector<EpdKernClassEntry> kernLeft;
  std::vector<EpdKernClassEntry> kernRight;
  std::vector<int8_t> kernMatrix;
  std::vector<EpdLigaturePair> ligatures;

  EpdFontData data{};

  // Carried from the header's kExternalFontFlagTwoBit. It is a member rather than a constant
  // because wireData() runs after parsing and must publish the value the BLOB declares, not the
  // value this translation unit happens to assume: the format is meant to hold 1-bit faces too,
  // and a loader that hardcodes 2-bit silently mis-decodes every one of them.
  bool twoBit = true;

  ExternalBuiltinFont() = default;
  ExternalBuiltinFont(const ExternalBuiltinFont&) = delete;
  ExternalBuiltinFont& operator=(const ExternalBuiltinFont&) = delete;

  // Point EpdFontData at the owned storage. Must be called after the vectors are final — a later
  // resize would invalidate every pointer here.
  void wireData() {
    data.bitmap = bitmap.data();
    data.glyph = glyphs.data();
    data.intervals = intervals.data();
    data.intervalCount = static_cast<uint32_t>(intervals.size());
    data.is2Bit = twoBit;
    data.groups = nullptr;
    data.groupCount = 0;
    data.glyphToGroup = nullptr;
    data.kernLeftClasses = kernLeft.empty() ? nullptr : kernLeft.data();
    data.kernRightClasses = kernRight.empty() ? nullptr : kernRight.data();
    data.kernMatrix = kernMatrix.empty() ? nullptr : kernMatrix.data();
    data.kernLeftEntryCount = static_cast<uint16_t>(kernLeft.size());
    data.kernRightEntryCount = static_cast<uint16_t>(kernRight.size());
    data.ligaturePairs = ligatures.empty() ? nullptr : ligatures.data();
    data.ligaturePairCount = static_cast<uint32_t>(ligatures.size());
  }
};

namespace detail {
inline bool inRange(size_t offset, size_t count, size_t elemSize, size_t total) {
  if (elemSize != 0 && count > (SIZE_MAX / elemSize)) return false;  // overflow
  const size_t bytes = count * elemSize;
  return offset <= total && bytes <= total - offset;
}
}  // namespace detail

// Parse `len` bytes at `bytes` into `out`. On failure returns false and sets `err`; `out` is untouched
// in that case (parsing into a local and swapping at the end keeps the failure path obviously clean).
inline bool parseExternalFont(const uint8_t* bytes, size_t len, std::unique_ptr<ExternalBuiltinFont>& out,
                              std::string& err) {
  auto fail = [&err](const char* msg) { err = msg; return false; };
  if (bytes == nullptr) return fail("null blob");
  if (len < sizeof(ExternalFontHeader)) return fail("blob shorter than the header");

  ExternalFontHeader h{};
  std::memcpy(&h, bytes, sizeof(h));
  if (h.magic != kExternalFontMagic) return fail("bad magic (expected EPD2)");
  if (h.version != kExternalFontVersion) return fail("unsupported version");
  if (h.totalBytes != len) return fail("totalBytes does not match the buffer length");
  if (h.glyphCount == 0 || h.intervalCount == 0 || h.bitmapBytes == 0) return fail("empty font");

  // Reject unknown flags rather than ignoring them. A flag this build does not understand is a
  // blob written by a newer exporter, and the safe reading of "there is more here than I know
  // about" is to refuse: silently proceeding means decoding sections whose layout may have moved.
  constexpr uint16_t kKnownFlags =
      kExternalFontFlagTwoBit | kExternalFontFlagHasKern | kExternalFontFlagHasLigatures;
  if ((h.flags & ~kKnownFlags) != 0) return fail("unsupported EPD2 flags");

  const bool isTwoBit = (h.flags & kExternalFontFlagTwoBit) != 0;
  const bool hasKern = (h.flags & kExternalFontFlagHasKern) != 0;
  const bool hasLig = (h.flags & kExternalFontFlagHasLigatures) != 0;

  // Bounds-check every section against the buffer before allocating anything from it.
  if (!detail::inRange(h.intervalsOffset, h.intervalCount, sizeof(EpdUnicodeInterval), len))
    return fail("intervals out of range");
  if (!detail::inRange(h.glyphsOffset, h.glyphCount, sizeof(EpdGlyph), len))
    return fail("glyphs out of range");
  if (!detail::inRange(h.bitmapOffset, h.bitmapBytes, 1, len)) return fail("bitmap out of range");
  if (hasKern) {
    if (!detail::inRange(h.kernLeftOffset, h.kernLeftEntries, sizeof(EpdKernClassEntry), len))
      return fail("kern left map out of range");
    if (!detail::inRange(h.kernRightOffset, h.kernRightEntries, sizeof(EpdKernClassEntry), len))
      return fail("kern right map out of range");
    if (h.kernLeftClasses == 0 || h.kernRightClasses == 0) return fail("kern matrix has zero classes");
    const size_t cells = static_cast<size_t>(h.kernLeftClasses) * h.kernRightClasses;
    if (!detail::inRange(h.kernMatrixOffset, cells, sizeof(int8_t), len))
      return fail("kern matrix out of range");
  }
  if (hasLig && h.ligatureCount > 0 &&
      !detail::inRange(h.ligatureOffset, h.ligatureCount, sizeof(EpdLigaturePair), len))
    return fail("ligatures out of range");

  // No try/catch here: the emscripten target builds with -fno-exceptions, so an allocation failure
  // cannot be caught and would abort. The bounds checks above are the real guard — every section is
  // proven to fit inside the buffer before a single byte is reserved.
  auto bundle = std::make_unique<ExternalBuiltinFont>();
  {
    bundle->intervals.resize(h.intervalCount);
    std::memcpy(bundle->intervals.data(), bytes + h.intervalsOffset,
                h.intervalCount * sizeof(EpdUnicodeInterval));

    bundle->glyphs.resize(h.glyphCount);
    std::memcpy(bundle->glyphs.data(), bytes + h.glyphsOffset, h.glyphCount * sizeof(EpdGlyph));

    bundle->bitmap.resize(h.bitmapBytes);
    std::memcpy(bundle->bitmap.data(), bytes + h.bitmapOffset, h.bitmapBytes);

    if (hasKern) {
      bundle->kernLeft.resize(h.kernLeftEntries);
      std::memcpy(bundle->kernLeft.data(), bytes + h.kernLeftOffset,
                  h.kernLeftEntries * sizeof(EpdKernClassEntry));
      bundle->kernRight.resize(h.kernRightEntries);
      std::memcpy(bundle->kernRight.data(), bytes + h.kernRightOffset,
                  h.kernRightEntries * sizeof(EpdKernClassEntry));
      const size_t cells = static_cast<size_t>(h.kernLeftClasses) * h.kernRightClasses;
      bundle->kernMatrix.resize(cells);
      std::memcpy(bundle->kernMatrix.data(), bytes + h.kernMatrixOffset, cells);
    }

    if (hasLig && h.ligatureCount > 0) {
      bundle->ligatures.resize(h.ligatureCount);
      std::memcpy(bundle->ligatures.data(), bytes + h.ligatureOffset,
                  h.ligatureCount * sizeof(EpdLigaturePair));
    }
  }

  bundle->data.advanceY = h.advanceY;
  bundle->data.ascender = h.ascender;
  bundle->data.descender = h.descender;
  bundle->data.kernLeftClassCount = h.kernLeftClasses;
  bundle->data.kernRightClassCount = h.kernRightClasses;
  bundle->twoBit = isTwoBit;
  bundle->wireData();

  out = std::move(bundle);
  return true;
}

}  // namespace ko
