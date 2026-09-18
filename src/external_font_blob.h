// Lossless external built-in font blob ("EPD2").
//
// WHY NOT .epdfont v1: v1 stores advanceX as an 8-bit INTEGER pixel count, while the runtime glyph is
// 12.4 fixed-point in a uint16_t. Built-in KoPub has e.g. U+AC00 advanceX = 437, i.e. 27.3125 px; v1
// cannot represent the fraction, so converting through it changes wraps and pagination. v1 also loses
// kerning entirely (UnifiedFontFamily::getKerning returns 0 for SD fonts) and built-in KoPub ships a
// real 34x31 kern matrix. Visual parity with the ko fork requires both, so built-in fonts move as this
// format instead: the arrays are the RUNTIME types, copied verbatim, with no quantisation at all.
//
// WHAT THIS FORMAT IS NOT: it is not a compression format, not a subsetter, and not a re-encoder. The
// blob is a straight relocation of the arrays that today live in the wasm's data section.
//
// Layout: header, then each section at its recorded offset, each 4-byte aligned:
//   intervals (EpdUnicodeInterval[]), glyphs (EpdGlyph[]), bitmaps (uint8_t[]),
//   kernLeft (EpdKernClassEntry[]), kernRight (EpdKernClassEntry[]), kernMatrix (int8_t[]),
//   ligatures (EpdLigaturePair[])
//
// The records are the runtime structs, so the loader is a memcpy + pointer wiring, and the static
// asserts below freeze the record sizes that make that safe across toolchains.
#pragma once
#include <cstdint>
#include <cstddef>

// The runtime font vocabulary. Included for the record types and fp4:: helpers below.
#include <EpdFontData.h>

namespace ko {

constexpr uint32_t kExternalFontMagic = 0x32445045u;  // "EPD2" little-endian
constexpr uint16_t kExternalFontVersion = 2;

// Header flags.
constexpr uint16_t kExternalFontFlagTwoBit = 0x0001;  // data.is2Bit
constexpr uint16_t kExternalFontFlagHasKern = 0x0002;  // kern arrays are present
constexpr uint16_t kExternalFontFlagHasLigatures = 0x0004;

#pragma pack(push, 1)
struct ExternalFontHeader {
  uint32_t magic;    // kExternalFontMagic
  uint16_t version;  // kExternalFontVersion
  uint16_t flags;    // kExternalFontFlag*

  uint32_t intervalCount;
  uint32_t glyphCount;
  uint32_t bitmapBytes;

  uint32_t intervalsOffset;
  uint32_t glyphsOffset;
  uint32_t bitmapOffset;

  uint32_t kernLeftOffset;
  uint32_t kernRightOffset;
  uint32_t kernMatrixOffset;

  uint16_t kernLeftEntries;   // entries in the left class map
  uint16_t kernRightEntries;  // entries in the right class map
  uint8_t kernLeftClasses;    // matrix rows (distinct left classes)
  uint8_t kernRightClasses;   // matrix cols

  uint8_t advanceY;  // EpdFontData::advanceY (u8)
  // ascender/descender are `int` in EpdFontData. The first sketch for this format used int8_t, which
  // would silently truncate any font whose metrics exceed 127 — an unnecessary parity risk, so they are
  // stored wide. There is no size argument for narrowing: 6 bytes total.
  int32_t ascender;
  int32_t descender;

  uint32_t ligatureOffset;
  uint32_t ligatureCount;

  uint32_t totalBytes;  // must equal the size of the blob handed to the loader
};
#pragma pack(pop)

// The loader memcpys these records straight into std::vector<T>, so the on-disk stride must equal the
// runtime stride. EpdGlyph is NOT declared packed in EpdFontData.h (natural alignment, 2 tail bytes),
// which is exactly why it is copied whole rather than field by field.
static_assert(sizeof(EpdGlyph) == 16, "EpdGlyph layout changed: the EPD2 blob must be versioned");
static_assert(sizeof(EpdUnicodeInterval) == 12, "EpdUnicodeInterval layout changed");
static_assert(sizeof(EpdKernClassEntry) == 3, "EpdKernClassEntry is packed; layout changed");
static_assert(sizeof(EpdLigaturePair) == 8, "EpdLigaturePair is packed; layout changed");

// A font this format cannot represent must be refused, not silently degraded. KoPub satisfies all of
// these today; the checks exist so a future re-generated header fails loudly instead of dropping data.
inline bool externalFontRepresentable(const EpdFontData& d, const char** why) {
  if (d.groups != nullptr || d.groupCount != 0) {
    *why = "compressed groups (EpdFontGroup) are not supported";
    return false;
  }
  if (d.glyphToGroup != nullptr) {
    *why = "glyphToGroup is not supported";
    return false;
  }
  if (d.intervalCount == 0 || d.glyph == nullptr) {
    *why = "empty font";
    return false;
  }
  return true;
}

}  // namespace ko
