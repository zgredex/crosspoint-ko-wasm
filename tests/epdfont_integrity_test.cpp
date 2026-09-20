#include <HalStorage.h>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

#include "SdFont.h"
#include "SdFontFormat.h"

namespace {

template <typename T>
void append(std::vector<uint8_t>& out, const T& value) {
  const auto* p = reinterpret_cast<const uint8_t*>(&value);
  out.insert(out.end(), p, p + sizeof(T));
}

template <typename T>
void writeAt(std::vector<uint8_t>& out, size_t offset, const T& value) {
  std::memcpy(out.data() + offset, &value, sizeof(value));
}

std::vector<uint8_t> validFont() {
  EpdFontHeader header{};
  header.magic = EPDFONT_MAGIC;
  header.version = EPDFONT_VERSION;
  header.is2Bit = 1;
  header.advanceY = 20;
  header.ascender = 15;
  header.descender = -5;
  header.intervalCount = 2;
  header.glyphCount = 2;
  header.intervalsOffset = sizeof(EpdFontHeader);
  header.glyphsOffset = header.intervalsOffset + 2 * sizeof(EpdFontInterval);
  header.bitmapOffset = header.glyphsOffset + 2 * sizeof(EpdFontGlyph);

  EpdFontInterval first{0x41, 0x41, 0};
  EpdFontInterval second{0x43, 0x43, 1};
  EpdFontGlyph glyphA{1, 1, 1, 0, 0, 1, 1, 0};
  EpdFontGlyph glyphC{1, 1, 1, 0, 0, 1, 1, 1};

  std::vector<uint8_t> out;
  append(out, header);
  append(out, first);
  append(out, second);
  append(out, glyphA);
  append(out, glyphC);
  out.push_back(0x00);
  out.push_back(0xff);
  return out;
}

bool loads(const std::vector<uint8_t>& bytes) {
  Storage.clearAll();
  Storage.mountBlob("/font.epdfont", bytes);
  SdFontData font("/font.epdfont");
  return font.load();
}

bool reject(std::vector<uint8_t> bytes, const char* label) {
  if (!loads(bytes)) return true;
  std::cerr << "accepted malformed epdfont: " << label << '\n';
  return false;
}

}  // namespace

int main() {
  const std::vector<uint8_t> valid = validFont();
  if (!loads(valid)) {
    std::cerr << "canonical epdfont was rejected\n";
    return 1;
  }

  for (size_t n = 0; n < valid.size(); ++n) {
    if (!reject(std::vector<uint8_t>(valid.begin(), valid.begin() + n), "truncation")) return 1;
  }

  {
    auto bad = valid;
    EpdFontHeader h{};
    std::memcpy(&h, bad.data(), sizeof(h));
    h.intervalsOffset++;
    writeAt(bad, 0, h);
    if (!reject(std::move(bad), "interval offset")) return 1;
  }
  {
    auto bad = valid;
    EpdFontHeader h{};
    std::memcpy(&h, bad.data(), sizeof(h));
    h.glyphsOffset = 0xfffffff0u;
    writeAt(bad, 0, h);
    if (!reject(std::move(bad), "wrapped glyph offset")) return 1;
  }
  {
    auto bad = valid;
    EpdFontInterval second{};
    const size_t at = sizeof(EpdFontHeader) + sizeof(EpdFontInterval);
    std::memcpy(&second, bad.data() + at, sizeof(second));
    second.first = 0x41;
    writeAt(bad, at, second);
    if (!reject(std::move(bad), "overlapping intervals")) return 1;
  }
  {
    auto bad = valid;
    EpdFontInterval second{};
    const size_t at = sizeof(EpdFontHeader) + sizeof(EpdFontInterval);
    std::memcpy(&second, bad.data() + at, sizeof(second));
    second.offset = 0;
    writeAt(bad, at, second);
    if (!reject(std::move(bad), "duplicate glyph coverage")) return 1;
  }
  {
    auto bad = valid;
    EpdFontHeader h{};
    std::memcpy(&h, bad.data(), sizeof(h));
    EpdFontGlyph glyph{};
    std::memcpy(&glyph, bad.data() + h.glyphsOffset, sizeof(glyph));
    glyph.dataLength = 65536;
    writeAt(bad, h.glyphsOffset, glyph);
    if (!reject(std::move(bad), "unrepresentable glyph length")) return 1;
  }
  {
    auto bad = valid;
    EpdFontHeader h{};
    std::memcpy(&h, bad.data(), sizeof(h));
    EpdFontGlyph glyph{};
    std::memcpy(&glyph, bad.data() + h.glyphsOffset, sizeof(glyph));
    glyph.dataOffset = 0xffffffffu;
    writeAt(bad, h.glyphsOffset, glyph);
    if (!reject(std::move(bad), "bitmap span overflow")) return 1;
  }

  std::cout << "epdfont-integrity: canonical v1 loads; truncations, offsets, intervals and bitmap spans reject\n";
  return 0;
}
