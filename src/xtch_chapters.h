// xtch_chapters.h — chapter assembly from raw TOC candidates, shared by every export path.
//
// This is deliberately one implementation used by THREE callers: the serial wasm export
// (ko_export_finish), the pooled wasm assembler (ko_assemble_finish), and the host CLI's pooled
// reference path. The pooled gate's whole claim is "the assembled container is byte-identical to the
// serial one", and that claim is only meaningful if both sides run the same chapter logic. A second
// copy of this function would make the gate test the copy, not the product.
//
// Semantics:
//   * at most 100 entries (official MAX_TOC_EXPORT)
//   * EPUB navigation entries are the only source of chapter names
//   * then a STABLE sort by page, so equal-page entries keep TOC order
//   * same-page duplicates collapse (a sub-entry aliasing its parent's heading)
//   * each chapter ends one page before the next; the last ends at the final page
//   * no usable TOC entries → no chapter table; never invent names from XHTML or filenames
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "xtch_writer.h"

namespace ko {

inline constexpr size_t MAX_EXPORTED_CHAPTERS = 100;

// Keep collection bounded at the format/converter limit. Empty navigation
// labels are unusable and are skipped instead of being replaced with a made-up
// "Chapter N" value.
inline bool retainChapterCandidate(std::vector<ChapterCandidate>& candidates,
                                   ChapterCandidate candidate) {
  if (candidate.title.empty() || candidates.size() >= MAX_EXPORTED_CHAPTERS) return false;
  candidates.push_back(std::move(candidate));
  return true;
}

inline std::vector<XtchChapter> buildChapters(
    const std::vector<ChapterCandidate>& candIn,
    uint32_t totalPages) {
  std::vector<XtchChapter> out;
  const uint32_t lastPage = totalPages > 0 ? totalPages - 1 : 0;
  std::vector<ChapterCandidate> cand;
  cand.reserve(std::min(MAX_EXPORTED_CHAPTERS, candIn.size()));
  for (const auto& c : candIn) {
    if (cand.size() == MAX_EXPORTED_CHAPTERS) break;
    if (!c.title.empty()) cand.push_back(c);
  }
  std::stable_sort(cand.begin(), cand.end(),
                   [](const ChapterCandidate& a, const ChapterCandidate& b) {
                     return a.page < b.page;
                   });
  std::vector<ChapterCandidate> uniq;
  for (const auto& c : cand) {
    if (uniq.empty() || uniq.back().page != c.page) uniq.push_back(c);
  }
  if (!uniq.empty()) {
    for (size_t i = 0; i < uniq.size(); i++) {
      XtchChapter ch;
      ch.name = uniq[i].title;
      ch.startPage = static_cast<uint16_t>(uniq[i].page);
      ch.endPage = static_cast<uint16_t>((i + 1 < uniq.size()) ? uniq[i + 1].page - 1 : lastPage);
      if (ch.endPage < ch.startPage) ch.endPage = ch.startPage;
      if (ch.startPage <= lastPage) out.push_back(ch);
    }
  }
  return out;
}

}  // namespace ko
