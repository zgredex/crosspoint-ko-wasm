#include <HalStorage.h>
#include <HalDisplay.h>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "Epub/BookMetadataCache.h"
#include "Epub.h"
#include "Epub/Section.h"
#include "GfxRenderer.h"

HalDisplay display;  // engine image helpers use the firmware's global display symbol

namespace {

template <typename T>
void appendPod(std::vector<uint8_t>& out, const T& value) {
  const auto* p = reinterpret_cast<const uint8_t*>(&value);
  out.insert(out.end(), p, p + sizeof(T));
}

void appendString(std::vector<uint8_t>& out, const std::string& value) {
  const uint32_t size = static_cast<uint32_t>(value.size());
  appendPod(out, size);
  out.insert(out.end(), value.begin(), value.end());
}

void writeU32(std::vector<uint8_t>& out, size_t offset, uint32_t value) {
  std::memcpy(out.data() + offset, &value, sizeof(value));
}

std::vector<uint8_t> validCache() {
  std::vector<uint8_t> out;
  const uint8_t version = 10;
  appendPod(out, version);
  const size_t lutOffsetField = out.size();
  appendPod(out, uint32_t{0});
  appendPod(out, uint16_t{1});
  appendPod(out, uint16_t{1});
  appendString(out, "title");
  appendString(out, "author");
  appendString(out, "ko");
  appendString(out, "cover.jpg");
  appendString(out, "Text/a.xhtml");

  const uint32_t lutOffset = static_cast<uint32_t>(out.size());
  writeU32(out, lutOffsetField, lutOffset);
  const size_t spineLut = out.size();
  appendPod(out, uint32_t{0});
  const size_t tocLut = out.size();
  appendPod(out, uint32_t{0});

  const uint32_t spineOffset = static_cast<uint32_t>(out.size());
  appendString(out, "Text/a.xhtml");
  appendPod(out, uint32_t{123});
  appendPod(out, int16_t{0});

  const uint32_t tocOffset = static_cast<uint32_t>(out.size());
  appendString(out, "Chapter");
  appendString(out, "Text/a.xhtml");
  appendString(out, "start");
  appendPod(out, uint8_t{0});
  appendPod(out, int16_t{0});

  writeU32(out, spineLut, spineOffset);
  writeU32(out, tocLut, tocOffset);
  return out;
}

bool loadBytes(const std::vector<uint8_t>& bytes) {
  Storage.clearAll();
  Storage.mountBlob("/cache/book.bin", bytes);
  BookMetadataCache cache("/cache");
  return cache.load();
}

}  // namespace

int main() {
  const std::vector<uint8_t> valid = validCache();
  if (!loadBytes(valid)) {
    std::cerr << "valid cache was rejected\n";
    return 1;
  }

  Storage.clearAll();
  Storage.mountBlob("/cache/book.bin", valid);
  BookMetadataCache cache("/cache");
  if (!cache.load() || cache.getSpineCount() != 1 || cache.getTocCount() != 1 ||
      cache.getSpineEntry(0).href != "Text/a.xhtml" || cache.getTocEntry(0).title != "Chapter") {
    std::cerr << "valid cache contents did not round-trip\n";
    return 1;
  }

  for (size_t size = 0; size < valid.size(); ++size) {
    std::vector<uint8_t> truncated(valid.begin(), valid.begin() + size);
    if (loadBytes(truncated)) {
      std::cerr << "truncated cache accepted at " << size << "/" << valid.size() << " bytes\n";
      return 1;
    }
  }

  std::vector<uint8_t> badLut = valid;
  uint32_t lutOffset = 0;
  std::memcpy(&lutOffset, badLut.data() + 1, sizeof(lutOffset));
  writeU32(badLut, lutOffset, static_cast<uint32_t>(badLut.size() + 1));
  if (loadBytes(badLut)) {
    std::cerr << "out-of-range LUT pointer was accepted\n";
    return 1;
  }

  // Section cache accessors are callable independently of loadSectionFile()
  // (anchor/progress lookups do exactly that). Every header truncation must
  // therefore fail locally instead of consuming zero-initialized read output.
  constexpr size_t sectionHeaderSize =
      sizeof(uint8_t) + sizeof(int) + sizeof(float) + sizeof(bool) + sizeof(bool) + sizeof(uint8_t) +
      sizeof(bool) + sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint16_t) + sizeof(bool) + sizeof(bool) +
      sizeof(uint8_t) + sizeof(bool) + 5 * sizeof(uint32_t);
  GfxRenderer renderer(display);
  auto epub = std::make_shared<Epub>("/book.epub", "/cache");
  const std::string sectionPath = epub->getCachePath() + "/sections/0.bin";
  ReaderRenderSpec spec{};
  for (size_t size = 0; size < sectionHeaderSize; ++size) {
    Storage.clearAll();
    Storage.mountBlob(sectionPath, std::vector<uint8_t>(size, 0));
    {
      Section section(epub, 0, renderer);
      if (section.loadSectionFile(spec)) {
        std::cerr << "truncated section cache loaded at " << size << "/"
                  << sectionHeaderSize << " bytes\n";
        return 1;
      }
    }
    Storage.mountBlob(sectionPath, std::vector<uint8_t>(size, 0));
    Section section(epub, 0, renderer);
    if (section.getCachedPageCount() ||
        section.getPageForAnchor("x") || section.getPageForParagraphIndex(0) ||
        section.getPageForListItemIndex(0) || section.getParagraphIndexForPage(0) ||
        section.getVisibleTextOffsetForPage(0) || section.getPageForVisibleTextOffset(0)) {
      std::cerr << "truncated section cache accessor succeeded at " << size << "/"
                << sectionHeaderSize << " bytes\n";
      return 1;
    }
  }

  std::vector<uint8_t> badSectionHeader(sectionHeaderSize, 0);
  size_t headerPos = 0;
  const auto putHeader = [&badSectionHeader, &headerPos](const auto& value) {
    std::memcpy(badSectionHeader.data() + headerPos, &value, sizeof(value));
    headerPos += sizeof(value);
  };
  putHeader(uint8_t{42});
  putHeader(spec.fontId);
  putHeader(spec.lineCompression);
  putHeader(spec.extraParagraphSpacing);
  putHeader(spec.paragraphIndent);
  putHeader(spec.paragraphAlignment);
  putHeader(spec.characterWrap);
  putHeader(spec.viewportWidth);
  putHeader(spec.viewportHeight);
  putHeader(spec.hyphenationEnabled);
  putHeader(spec.embeddedStyle);
  putHeader(spec.imageRendering);
  putHeader(spec.focusReadingEnabled);
  putHeader(uint16_t{1});
  for (int i = 0; i < 5; ++i) putHeader(uint32_t{UINT32_MAX});
  if (headerPos != sectionHeaderSize) {
    std::cerr << "section header test fixture size drifted\n";
    return 1;
  }
  Storage.clearAll();
  Storage.mountBlob(sectionPath, badSectionHeader);
  {
    Section section(epub, 0, renderer);
    if (section.loadSectionFile(spec)) {
      std::cerr << "section cache with out-of-range LUT offsets loaded\n";
      return 1;
    }
  }
  Storage.mountBlob(sectionPath, badSectionHeader);
  {
    Section section(epub, 0, renderer);
    if (section.getPageForAnchor("x") || section.getPageForParagraphIndex(0) ||
        section.getPageForListItemIndex(0) || section.getParagraphIndexForPage(0) ||
        section.getVisibleTextOffsetForPage(0) || section.getPageForVisibleTextOffset(0)) {
      std::cerr << "section cache accessor accepted an out-of-range LUT offset\n";
      return 1;
    }
  }

  std::cout << "cache-integrity: metadata and section truncations plus bad LUTs are rejected\n";
  return 0;
}
