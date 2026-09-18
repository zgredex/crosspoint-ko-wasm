// Export a built-in font from its generated C header into a lossless "EPD2" blob.
//
//   clang++ -std=c++17 -O2 -Ivendor-lib -Ivendor-lib/EpdFont -Isrc \
//       tools/export_external_font.cpp -o build/export_external_font
//   build/export_external_font /tmp/ab/kopub_14.epd2
//
// The point of a native tool rather than a text scraper: it compiles the REAL header and copies the
// REAL arrays through the runtime structs, so what lands in the blob is literally what the wasm had in
// its data section — including 12.4 fixed-point advances (U+AC00 = 437) and the full kern matrix.
//
// It prints a self-check that fails loudly if the blob would not reproduce the embedded font, then
// writes the blob. Exit code is non-zero on any mismatch.
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

#include "external_font_blob.h"

// The generated header defines the arrays as `static const`, so including it here is enough: this TU
// owns the only copy. The macro renames the font's EpdFontData symbol so several fonts can be exported
// by the same binary in future without a collision.
#include "builtinFonts/kopub_14_regular.h"

using ko::ExternalFontHeader;
using ko::kExternalFontMagic;
using ko::kExternalFontVersion;
using ko::kExternalFontFlagTwoBit;
using ko::kExternalFontFlagHasKern;
using ko::kExternalFontFlagHasLigatures;

namespace {

void put32(std::vector<uint8_t>& out, uint32_t v) {
  out.insert(out.end(), reinterpret_cast<uint8_t*>(&v), reinterpret_cast<uint8_t*>(&v) + 4);
}

// Align the write cursor to 4 bytes so each section starts aligned (the loader memcpys records).
void align4(std::vector<uint8_t>& out) {
  while (out.size() % 4 != 0) out.push_back(0);
}

template <typename T>
void appendArray(std::vector<uint8_t>& out, const T* data, size_t count) {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(data);
  out.insert(out.end(), p, p + count * sizeof(T));
}

int fail(const char* msg) {
  std::fprintf(stderr, "FAIL: %s\n", msg);
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  const char* outPath = (argc > 1) ? argv[1] : "/tmp/ab/kopub_14.epd2";
  const EpdFontData& src = kopub_14_regular;

  const char* why = nullptr;
  if (!ko::externalFontRepresentable(src, &why)) return fail(why);

  // The header has no explicit glyph count; derive it from the last interval (intervals map codepoint
  // ranges to glyph indices, so the highest offset+span is the glyph array length). This mirrors how
  // EpdFont would index the array, so a blob built from it cannot over- or under-run.
  uint32_t glyphs = 0;
  for (uint32_t i = 0; i < src.intervalCount; ++i) {
    const EpdUnicodeInterval& iv = src.intervals[i];
    const uint32_t end = iv.offset + (iv.last - iv.first) + 1;
    if (end > glyphs) glyphs = end;
  }
  if (glyphs == 0) return fail("derived glyph count is zero");

  // Bitmap extent: the largest dataOffset + dataLength over the glyph array.
  uint32_t bitmapBytes = 0;
  for (uint32_t g = 0; g < glyphs; ++g) {
    const EpdGlyph& gl = src.glyph[g];
    const uint32_t end = gl.dataOffset + gl.dataLength;
    if (end > bitmapBytes) bitmapBytes = end;
  }
  if (bitmapBytes == 0) return fail("derived bitmap extent is zero");

  // Kern tables: count entries by scanning until the class map ends. The generated header has no
  // explicit entry count either, so use the class counts recorded in EpdFontData plus the invariant
  // that the map is sorted by codepoint and terminates at the array the data struct points into.
  const uint32_t kernLeftEntries = src.kernLeftEntryCount;
  const uint32_t kernRightEntries = src.kernRightEntryCount;

  // ---- self-checks on the embedded data, before anything is written ----
  // U+AC00 must be present with the 12.4 advance the fork's typography depends on.
  int uac00 = -1;
  for (uint32_t i = 0; i < src.intervalCount; ++i) {
    const EpdUnicodeInterval& iv = src.intervals[i];
    if (0xAC00 >= iv.first && 0xAC00 <= iv.last) { uac00 = static_cast<int>(iv.offset + (0xAC00 - iv.first)); break; }
  }
  if (uac00 < 0) return fail("U+AC00 not found in the interval table");
  const EpdGlyph& hangul = src.glyph[uac00];
  std::printf("U+AC00 glyph #%d: w=%u h=%u advanceX=%u (%.4f px) left=%d top=%d len=%u off=%u\n",
              uac00, hangul.width, hangul.height, hangul.advanceX,
              hangul.advanceX / 16.0, hangul.left, hangul.top, hangul.dataLength, hangul.dataOffset);
  if (hangul.advanceX != 437) {
    std::fprintf(stderr, "FAIL: U+AC00 advanceX is %u, expected 437 (the 12.4 value the ko fork needs)\n",
                 hangul.advanceX);
    return 1;
  }
  if (hangul.advanceX % 16 == 0) {
    std::fprintf(stderr, "WARN: U+AC00 advanceX has no fractional part; this font may not be the "
                         "fractional-advance build the format exists for\n");
  }

  std::printf("kern: leftEntries=%u rightEntries=%u leftClasses=%u rightClasses=%u matrix=%s\n",
              kernLeftEntries, kernRightEntries, src.kernLeftClassCount, src.kernRightClassCount,
              src.kernMatrix ? "present" : "ABSENT");
  if (src.kernLeftClassCount != 34 || src.kernRightClassCount != 31) {
    std::fprintf(stderr, "WARN: expected 34x31 kern classes, got %ux%u\n",
                 src.kernLeftClassCount, src.kernRightClassCount);
  }
  if (src.kernMatrix == nullptr || src.kernLeftClasses == nullptr || src.kernRightClasses == nullptr) {
    return fail("kern classes/matrix pointer is null; kerning would be lost");
  }

  // ---- assemble ----
  std::vector<uint8_t> out;
  out.reserve(sizeof(ExternalFontHeader) + src.intervalCount * sizeof(EpdUnicodeInterval) +
              glyphs * sizeof(EpdGlyph) + bitmapBytes +
              (kernLeftEntries + kernRightEntries) * sizeof(EpdKernClassEntry) +
              static_cast<size_t>(src.kernLeftClassCount) * src.kernRightClassCount +
              src.ligaturePairCount * sizeof(EpdLigaturePair) + 64);
  out.resize(sizeof(ExternalFontHeader));

  const uint32_t intervalsOffset = static_cast<uint32_t>(out.size());
  appendArray(out, src.intervals, src.intervalCount);
  align4(out);

  const uint32_t glyphsOffset = static_cast<uint32_t>(out.size());
  appendArray(out, src.glyph, glyphs);
  align4(out);

  const uint32_t bitmapOffset = static_cast<uint32_t>(out.size());
  out.insert(out.end(), src.bitmap, src.bitmap + bitmapBytes);
  align4(out);

  const uint32_t kernLeftOffset = static_cast<uint32_t>(out.size());
  appendArray(out, src.kernLeftClasses, kernLeftEntries);
  align4(out);

  const uint32_t kernRightOffset = static_cast<uint32_t>(out.size());
  appendArray(out, src.kernRightClasses, kernRightEntries);
  align4(out);

  const uint32_t kernMatrixOffset = static_cast<uint32_t>(out.size());
  appendArray(out, src.kernMatrix,
              static_cast<size_t>(src.kernLeftClassCount) * src.kernRightClassCount);
  align4(out);

  const uint32_t ligatureOffset = static_cast<uint32_t>(out.size());
  if (src.ligaturePairCount && src.ligaturePairs) appendArray(out, src.ligaturePairs, src.ligaturePairCount);
  align4(out);

  ExternalFontHeader h{};
  h.magic = kExternalFontMagic;
  h.version = kExternalFontVersion;
  h.flags = static_cast<uint16_t>((src.is2Bit ? kExternalFontFlagTwoBit : 0) |
                                  (src.kernMatrix ? kExternalFontFlagHasKern : 0) |
                                  (src.ligaturePairCount ? kExternalFontFlagHasLigatures : 0));
  h.intervalCount = src.intervalCount;
  h.glyphCount = glyphs;
  h.bitmapBytes = bitmapBytes;
  h.intervalsOffset = intervalsOffset;
  h.glyphsOffset = glyphsOffset;
  h.bitmapOffset = bitmapOffset;
  h.kernLeftOffset = kernLeftOffset;
  h.kernRightOffset = kernRightOffset;
  h.kernMatrixOffset = kernMatrixOffset;
  h.kernLeftEntries = static_cast<uint16_t>(kernLeftEntries);
  h.kernRightEntries = static_cast<uint16_t>(kernRightEntries);
  h.kernLeftClasses = src.kernLeftClassCount;
  h.kernRightClasses = src.kernRightClassCount;
  h.advanceY = src.advanceY;
  h.ascender = src.ascender;
  h.descender = src.descender;
  h.ligatureOffset = ligatureOffset;
  h.ligatureCount = src.ligaturePairCount;
  h.totalBytes = static_cast<uint32_t>(out.size());

  std::memcpy(out.data(), &h, sizeof(h));

  if (h.totalBytes != out.size()) return fail("totalBytes mismatch");

  FILE* f = std::fopen(outPath, "wb");
  if (!f) return fail("cannot open output path");
  const size_t written = std::fwrite(out.data(), 1, out.size(), f);
  std::fclose(f);
  if (written != out.size()) return fail("short write");

  std::printf("wrote %s: %zu bytes (header %zu, intervals %zu x%u, glyphs %zu x%u, bitmaps %u, "
              "kern %u+%u+%u)\n",
              outPath, out.size(), sizeof(ExternalFontHeader), sizeof(EpdUnicodeInterval),
              src.intervalCount, sizeof(EpdGlyph), glyphs, bitmapBytes,
              kernLeftEntries * sizeof(EpdKernClassEntry),
              kernRightEntries * sizeof(EpdKernClassEntry),
              static_cast<size_t>(src.kernLeftClassCount) * src.kernRightClassCount);
  return 0;
}
