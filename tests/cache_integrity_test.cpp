#include <HalStorage.h>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "Epub/BookMetadataCache.h"

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

  std::cout << "cache-integrity: valid cache round-trips; every truncation and bad LUT is rejected\n";
  return 0;
}
