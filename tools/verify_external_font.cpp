// Round-trip proof for the EPD2 path: parse the blob with the SHIPPING loader (external_font_loader.h)
// and compare the result against the embedded arrays, field by field.
//
//   clang++ -std=c++17 -O2 -Ivendor-lib -Ivendor-lib/EpdFont -Isrc \
//       tools/verify_external_font.cpp -o build/verify_external_font
//   build/verify_external_font /tmp/ab/kopub_14.epd2
//
// This is the gate that would catch a lossy conversion: it is not enough for the glyph COUNT to match,
// or for the blob to be the right size. Every advanceX must be bit-identical (U+AC00 = 437, not 27<<4),
// the bitmap bytes must match exactly, and the full kern matrix must be present and equal — that last
// one is what .epdfont v1 cannot do at all.
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "external_font_loader.h"

#include "builtinFonts/kopub_14_regular.h"

namespace {

int failures = 0;

void check(bool ok, const char* what) {
  if (!ok) { std::fprintf(stderr, "MISMATCH: %s\n", what); ++failures; }
}

int fnv1a(const uint8_t* p, size_t n) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 16777619u; }
  return static_cast<int>(h);
}

}  // namespace

int main(int argc, char** argv) {
  const char* blobPath = (argc > 1) ? argv[1] : "/tmp/ab/kopub_14.epd2";

  std::ifstream f(blobPath, std::ios::binary);
  if (!f) { std::fprintf(stderr, "cannot open %s\n", blobPath); return 2; }
  std::vector<uint8_t> blob((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (blob.empty()) { std::fprintf(stderr, "empty blob\n"); return 2; }

  std::unique_ptr<ko::ExternalBuiltinFont> ext;
  std::string err;
  if (!ko::parseExternalFont(blob.data(), blob.size(), ext, err)) {
    std::fprintf(stderr, "PARSE FAILED: %s\n", err.c_str());
    return 1;
  }

  const EpdFontData& orig = kopub_14_regular;

  // ---- intervals ----
  check(ext->intervals.size() == orig.intervalCount, "interval count");
  for (size_t i = 0; i < ext->intervals.size() && i < orig.intervalCount; ++i) {
    if (std::memcmp(&ext->intervals[i], &orig.intervals[i], sizeof(EpdUnicodeInterval)) != 0) {
      check(false, "interval record");
      break;
    }
  }

  // ---- glyphs: derive the same glyph count the exporter did, then compare every record ----
  uint32_t glyphs = 0;
  for (uint32_t i = 0; i < orig.intervalCount; ++i) {
    const uint32_t end = orig.intervals[i].offset + (orig.intervals[i].last - orig.intervals[i].first) + 1;
    if (end > glyphs) glyphs = end;
  }
  check(ext->glyphs.size() == glyphs, "glyph count");
  size_t glyphMismatch = 0, advanceMismatch = 0;
  for (size_t g = 0; g < ext->glyphs.size() && g < glyphs; ++g) {
    if (std::memcmp(&ext->glyphs[g], &orig.glyph[g], sizeof(EpdGlyph)) != 0) ++glyphMismatch;
    if (ext->glyphs[g].advanceX != orig.glyph[g].advanceX) ++advanceMismatch;
  }
  check(glyphMismatch == 0, "glyph records differ");
  check(advanceMismatch == 0, "advanceX values differ (fractional advances lost)");

  // ---- the datum the whole format exists for ----
  int uac00 = -1;
  for (size_t i = 0; i < ext->intervals.size(); ++i) {
    if (0xAC00 >= ext->intervals[i].first && 0xAC00 <= ext->intervals[i].last) {
      uac00 = static_cast<int>(ext->intervals[i].offset + (0xAC00 - ext->intervals[i].first)); break;
    }
  }
  check(uac00 >= 0, "U+AC00 interval present");
  if (uac00 >= 0) {
    const EpdGlyph& g = ext->glyphs[uac00];
    std::printf("U+AC00 via blob: advanceX=%u (%.4f px) w=%u h=%u left=%d top=%d len=%u off=%u\n",
                g.advanceX, g.advanceX / 16.0, g.width, g.height, g.left, g.top, g.dataLength, g.dataOffset);
    check(g.advanceX == 437, "U+AC00 advanceX == 437");
    check(g.advanceX == orig.glyph[uac00].advanceX, "U+AC00 advanceX equals the embedded value");
  }

  // ---- bitmap ----
  uint32_t bitmapBytes = 0;
  for (uint32_t g = 0; g < glyphs; ++g) {
    const uint32_t end = orig.glyph[g].dataOffset + orig.glyph[g].dataLength;
    if (end > bitmapBytes) bitmapBytes = end;
  }
  check(ext->bitmap.size() == bitmapBytes, "bitmap extent");
  check(std::memcmp(ext->bitmap.data(), orig.bitmap, bitmapBytes) == 0, "bitmap bytes differ");

  // ---- kerning: the half of this that .epdfont v1 loses outright ----
  check(ext->kernLeft.size() == orig.kernLeftEntryCount, "kern left entry count");
  check(ext->kernRight.size() == orig.kernRightEntryCount, "kern right entry count");
  check(std::memcmp(ext->kernLeft.data(), orig.kernLeftClasses,
                    orig.kernLeftEntryCount * sizeof(EpdKernClassEntry)) == 0, "kern left map differs");
  check(std::memcmp(ext->kernRight.data(), orig.kernRightClasses,
                    orig.kernRightEntryCount * sizeof(EpdKernClassEntry)) == 0, "kern right map differs");
  const size_t cells = static_cast<size_t>(orig.kernLeftClassCount) * orig.kernRightClassCount;
  check(ext->kernMatrix.size() == cells, "kern matrix cell count");
  check(std::memcmp(ext->kernMatrix.data(), orig.kernMatrix, cells) == 0, "kern matrix differs");
  check(ext->data.kernLeftClassCount == orig.kernLeftClassCount, "kern left class count");
  check(ext->data.kernRightClassCount == orig.kernRightClassCount, "kern right class count");
  check(ext->data.kernMatrix != nullptr, "kern matrix pointer wired");

  // ---- metrics + wiring ----
  check(ext->data.advanceY == orig.advanceY, "advanceY");
  check(ext->data.ascender == orig.ascender, "ascender");
  check(ext->data.descender == orig.descender, "descender");
  check(ext->data.is2Bit == orig.is2Bit, "is2Bit");
  check(ext->data.intervalCount == orig.intervalCount, "wired intervalCount");
  check(ext->data.bitmap == ext->bitmap.data(), "bitmap pointer wired");
  check(ext->data.glyph == ext->glyphs.data(), "glyph pointer wired");
  check(ext->data.intervals == ext->intervals.data(), "intervals pointer wired");
  check(ext->data.groups == nullptr && ext->data.glyphToGroup == nullptr, "no group pointers");

  // A single hash over everything a renderer can reach, for a one-number comparison in the report.
  const int gh = fnv1a(reinterpret_cast<const uint8_t*>(ext->glyphs.data()),
                       ext->glyphs.size() * sizeof(EpdGlyph));
  const int bh = fnv1a(ext->bitmap.data(), ext->bitmap.size());
  const int kh = fnv1a(reinterpret_cast<const uint8_t*>(ext->kernMatrix.data()), ext->kernMatrix.size());

  std::printf("blob %zu bytes -> intervals %zu, glyphs %zu, bitmap %u, kern %zu+%zu+%zu cells\n",
              blob.size(), ext->intervals.size(), ext->glyphs.size(), ext->bitmap.size(),
              ext->kernLeft.size(), ext->kernRight.size(), ext->kernMatrix.size());
  std::printf("hashes glyphs=%d bitmap=%d kernMatrix=%d\n", gh, bh, kh);
  std::printf("%s\n", failures == 0 ? "IDENTICAL: external blob reproduces the embedded font bit-for-bit"
                                    : "FAILED: see mismatches above");
  return failures == 0 ? 0 : 1;
}
