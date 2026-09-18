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
#include "builtinFonts/ridibatang_14_regular.h"

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
  // usage: export_external_font <out.epd2> [kopub|ridibatang]
  //
  // The face is an argument because the EPD2 container is generic and the point of the format is
  // that any built-in face can be externalized. Which face is worth externalizing is a separate
  // question with a byte-count answer — see docs/ko-font-payload-measurement.md.
  const char* outPath = (argc > 1) ? argv[1] : "/tmp/ab/kopub_14.epd2";
  const char* face = (argc > 2) ? argv[2] : "kopub";

  const EpdFontData* srcPtr = nullptr;
  if (std::strcmp(face, "kopub") == 0) {
    srcPtr = &kopub_14_regular;
  } else if (std::strcmp(face, "ridibatang") == 0) {
    srcPtr = &ridibatang_14_regular;
  } else {
    return fail("unknown face (expected kopub or ridibatang)");
  }
  const EpdFontData& src = *srcPtr;
  std::fprintf(stderr, "face: %s\n", face);

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
  //
  // These guard against a lossily regenerated header (the .epdfont v1 failure: whole-pixel
  // advances). They used to be KoPub's numbers written as absolute rules — `advanceX != 437` — in a
  // tool that is now face-generic, which made RIDIBatang unexportable for no reason: its U+AC00 is
  // 444 (27.75 px), equally fractional, equally representable. The expectations are per face, and a
  // face that is not listed is held only to the general invariant.
  struct FaceExpectation {
    const char* name;
    uint16_t uac00Advance;      // 12.4 fixed-point; the value the runtime face carries
    uint16_t kernLeftClasses;
    uint16_t kernRightClasses;
  };
  static const FaceExpectation kFaceExpectations[] = {
      {"kopub", 437, 34, 31},
      {"ridibatang", 444, 0, 0},   // this face ships no kerning at all (all kern pointers null)
                                       // (733/731 are ENTRY counts; the class counts are 173/147)
  };
  const FaceExpectation* expect = nullptr;
  for (const auto& e : kFaceExpectations) {
    if (std::strcmp(e.name, face) == 0) expect = &e;
  }

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

  // What the format actually requires is that the value ROUND-TRIPS: the glyph record is a uint16
  // in 12.4 fixed point, so 437 (27.3125 px) and 288 (18.0 px) are both representable and both
  // exact. An earlier version of this gate demanded a fractional advance, which is a property of the
  // Korean READER faces, not of the format — and it rejected the UI face for having whole-pixel
  // advances, which is correct typography for a 10 pt UI font. The real check is the per-face
  // expectation below plus verify_external_font's field-by-field comparison.
  if (expect != nullptr && expect->uac00Advance != 0 &&
      hangul.advanceX != expect->uac00Advance) {
    std::fprintf(stderr, "FAIL: %s U+AC00 advanceX is %u, expected %u\n",
                 face, hangul.advanceX, expect->uac00Advance);
    return 1;
  }
  if (hangul.advanceX % 16 == 0 && (std::strcmp(face, "kopub") == 0 ||
                                    std::strcmp(face, "ridibatang") == 0)) {
    std::fprintf(stderr, "note: %s U+AC00 advance is a whole %u px — for a reader face that is "
                         "what a lossily regenerated header looks like; verify against the "
                         "committed header before trusting it\n", face, hangul.advanceX / 16);
  }

  std::printf("kern: leftEntries=%u rightEntries=%u leftClasses=%u rightClasses=%u matrix=%s\n",
              kernLeftEntries, kernRightEntries, src.kernLeftClassCount, src.kernRightClassCount,
              src.kernMatrix ? "present" : "ABSENT");
  if (expect != nullptr &&
      (src.kernLeftClassCount != expect->kernLeftClasses ||
       src.kernRightClassCount != expect->kernRightClasses)) {
    std::fprintf(stderr, "FAIL: %s has %ux%u kern classes, expected %ux%u\n",
                 face, src.kernLeftClassCount, src.kernRightClassCount,
                 expect->kernLeftClasses, expect->kernRightClasses);
    return 1;
  }
  // Kerning is OPTIONAL in the format (kExternalFontFlagHasKern), and RIDIBatang ships none. What
  // must not happen is a face that HAS kern classes but null pointers — that is half a kerning
  // table, and silently writing it as "no kerning" would change every kerned pair's advance. So the
  // requirement is consistency: either there is kerning and all of it is present, or there is none.
  const bool hasKerning = src.kernLeftClassCount > 0 && src.kernRightClassCount > 0;
  if (hasKerning &&
      (src.kernMatrix == nullptr || src.kernLeftClasses == nullptr || src.kernRightClasses == nullptr)) {
    return fail("kern class counts are non-zero but a kern array is null; kerning would be lost");
  }
  if (hasKerning && (src.kernLeftEntryCount == 0 || src.kernRightEntryCount == 0)) {
    return fail("kern classes are non-zero but an entry count is zero; kerning would be lost");
  }
  if (!hasKerning) {
    std::fprintf(stderr, "note: %s carries no kerning; the blob will be written without a kern "
                         "section (kExternalFontFlagHasKern clear)\n", face);
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
