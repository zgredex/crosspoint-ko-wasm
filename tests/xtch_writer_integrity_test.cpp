#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "xtch_writer.h"
#include "xtch_chapters.h"
#include "chapter_title.h"
#include "Epub/Section.h"

namespace {

bool fieldIsValidUtf8(const std::vector<uint8_t>& bytes, size_t offset, size_t fieldBytes) {
  size_t n = 0;
  while (n < fieldBytes && bytes[offset + n] != 0) ++n;
  if (n == fieldBytes) return false;
  return ko::utf8SafePrefixLength(reinterpret_cast<const char*>(bytes.data() + offset), n, n) == n;
}

std::string repeat(const char* utf8, int count) {
  std::string out;
  while (count-- > 0) out += utf8;
  return out;
}

}  // namespace

int main() {
  const std::string normalizedLong = ko::normalizeChapterTitle(repeat("가", 400));
  if (normalizedLong.size() > 1024 ||
      ko::utf8SafePrefixLength(normalizedLong, normalizedLong.size()) != normalizedLong.size()) {
    std::cerr << "normalized chapter title was cut inside UTF-8\n";
    return 1;
  }

  ko::XtchWriter writer(ko::XtcMode::Gray2Bit, ko::DeviceProfile::X4);
  writer.setMetadata(repeat("가", 60), repeat("나", 40), repeat("다", 30), "ko");
  ko::XtchChapter chapter;
  chapter.name = repeat("장", 40);
  chapter.startPage = 0;
  chapter.endPage = 0;
  const auto prefix = writer.buildPrefix({chapter}, {96022});
  if (prefix.empty() || !fieldIsValidUtf8(prefix, 56, 128) ||
      !fieldIsValidUtf8(prefix, 56 + 128, 64) || !fieldIsValidUtf8(prefix, 56 + 192, 32) ||
      !fieldIsValidUtf8(prefix, 312, 80)) {
    std::cerr << "fixed metadata/chapter fields are not terminated valid UTF-8\n";
    return 1;
  }
  if (std::strlen(reinterpret_cast<const char*>(prefix.data() + 56)) != 126 ||
      std::strlen(reinterpret_cast<const char*>(prefix.data() + 312)) != 78) {
    std::cerr << "Korean fields did not stop at the last complete Hangul syllable\n";
    return 1;
  }

  if (!writer.buildPrefix({}, {}).empty()) {
    std::cerr << "zero-page prefix was accepted\n";
    return 1;
  }

  {
    std::vector<ko::ChapterCandidate> candidates;
    candidates.push_back({"Navigation 1", 0});
    candidates.push_back({"", 1});
    candidates.push_back({"Navigation 2", 50});
    const auto chapters = ko::buildChapters(candidates, 200);
    if (chapters.size() != 2 || chapters[0].name != "Navigation 1" ||
        chapters[1].name != "Navigation 2") {
      std::cerr << "empty TOC titles were replaced or retained\n";
      return 1;
    }
  }
  {
    std::vector<ko::ChapterCandidate> retained;
    for (uint32_t page = 0; page < 150; ++page) {
      ko::retainChapterCandidate(retained, {"Navigation " + std::to_string(page), page});
    }
    const auto chapters = ko::buildChapters(retained, 200);
    if (retained.size() != 100 || chapters.size() != 100) {
      std::cerr << "TOC candidate collector did not enforce the 100-entry limit\n";
      return 1;
    }
  }
  {
    const auto chapters = ko::buildChapters({}, 200);
    if (!chapters.empty()) {
      std::cerr << "a book without a usable TOC received invented chapters\n";
      return 1;
    }
  }
  if (Section::canAppendSectionPage(Section::MAX_SECTION_PAGES) ||
      !Section::canAppendSectionPage(Section::MAX_SECTION_PAGES - 1)) {
    std::cerr << "section page-count serialization boundary is not enforced\n";
    return 1;
  }

  std::vector<uint8_t> record(22 + 96000, 0);
  record[0] = 'X'; record[1] = 'T'; record[2] = 'H';
  record[4] = 0xe0; record[5] = 0x01;  // 480
  record[6] = 0x20; record[7] = 0x03;  // 800
  record[10] = 0x00; record[11] = 0x77; record[12] = 0x01;  // 96000
  if (!writer.addRawPage(record.data(), record.size()) || writer.residentPageBytes() < record.size()) {
    std::cerr << "canonical raw page was refused or not accounted\n";
    return 1;
  }
  auto finished = writer.finish({chapter});
  if (finished.size() != 56 + 256 + 96 + 16 + record.size() || writer.residentPageBytes() != 0) {
    std::cerr << "finalization size/accounting is inconsistent\n";
    return 1;
  }

  std::cout << "xtch-writer-integrity: UTF-8 fields, empty refusal and page-memory release pass\n";
  return 0;
}
