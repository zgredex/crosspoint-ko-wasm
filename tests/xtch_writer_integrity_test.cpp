#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "xtch_writer.h"
#include "xtch_chapters.h"
#include "chapter_title_probe.h"
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
  {
    const std::string xhtml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?><html><head><title>Generic Section</title></head>"
        "<body><h1 style=\"display: none\">Hidden title</h1><h1>Section 1</h1>"
        "<h2 id=\"real\"><span>제1장</span> 진짜 이름 &amp; 시작</h2><p>text</p></body></html>";
    ko::ChapterTitleProbe probe;
    probe.write(reinterpret_cast<const uint8_t*>(xhtml.data()), xhtml.size());
    if (probe.headingTitle() != "제1장 진짜 이름 & 시작") {
      std::cerr << "streaming XHTML heading probe did not recover the visible title\n";
      return 1;
    }
  }
  {
    const std::string xhtml(512, 'x');
    ko::ChapterTitleProbe probe(64);
    probe.write(reinterpret_cast<const uint8_t*>(xhtml.data()), xhtml.size());
    if (probe.consumedBytes() != 64) {
      std::cerr << "streaming title probe did not enforce its injected byte limit\n";
      return 1;
    }
  }
  {
    const std::string malformed = "<html><body></html>";
    ko::ChapterTitleProbe probe(64);
    probe.write(reinterpret_cast<const uint8_t*>(malformed.data()), malformed.size());
    const size_t charged = probe.consumedBytes();
    probe.write(reinterpret_cast<const uint8_t*>(malformed.data()), malformed.size());
    if (charged != malformed.size() || probe.consumedBytes() != charged) {
      std::cerr << "streaming title probe did not charge the malformed terminal chunk exactly once\n";
      return 1;
    }
  }
  if (!ko::isGenericChapterTitle("Chapter IV") || !ko::isGenericChapterTitle("Section 12") ||
      !ko::isGenericChapterTitle("제12장") || !ko::isGenericChapterTitle("섹션 3") ||
      ko::isGenericChapterTitle("제12장 실제 이름") || ko::isGenericChapterTitle("Chapter Four Winds") ||
      ko::isGenericChapterTitle("XTCKO chapter parsing")) {
    std::cerr << "generic chapter-title classification is not conservative\n";
    return 1;
  }
  {
    const std::string xhtml = "<html><body><h1>First real heading</h1><h2>Later heading</h2></body></html>";
    ko::ChapterTitleProbe probe;
    probe.write(reinterpret_cast<const uint8_t*>(xhtml.data()), xhtml.size());
    if (probe.headingTitle() != "First real heading") {
      std::cerr << "streaming title probe did not stop at the first meaningful heading\n";
      return 1;
    }
  }
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
    candidates.push_back({"First real", 0, false});
    for (uint32_t page = 1; page <= 99; ++page) {
      candidates.push_back({"Inferred " + std::to_string(page), page, true});
    }
    candidates.push_back({"Late real", 150, false});
    const auto chapters = ko::buildChapters(candidates, {}, 200);
    const bool keptLateReal = std::any_of(chapters.begin(), chapters.end(), [](const auto& chapter) {
      return chapter.name == "Late real" && chapter.startPage == 150;
    });
    if (chapters.size() != 100 || !keptLateReal) {
      std::cerr << "inferred headings displaced an authoritative navigation entry\n";
      return 1;
    }
  }
  {
    std::vector<ko::ChapterCandidate> retained;
    for (uint32_t page = 0; page < 150; ++page) {
      ko::retainChapterCandidate(retained, {"Inferred " + std::to_string(page), page, true});
    }
    ko::retainChapterCandidate(retained, {"Late real", 150, false});
    const auto chapters = ko::buildChapters(retained, {}, 200);
    const bool keptLateReal = std::any_of(chapters.begin(), chapters.end(), [](const auto& chapter) {
      return chapter.name == "Late real" && chapter.startPage == 150;
    });
    if (retained.size() != 101 || chapters.size() != 100 || !keptLateReal) {
      std::cerr << "bounded candidate collector displaced a late authoritative entry\n";
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
