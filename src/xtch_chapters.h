// xtch_chapters.h — chapter assembly from raw TOC candidates, shared by every export path.
//
// This is deliberately one implementation used by THREE callers: the serial wasm export
// (ko_export_finish), the pooled wasm assembler (ko_assemble_finish), and the host CLI's pooled
// reference path. The pooled gate's whole claim is "the assembled container is byte-identical to the
// serial one", and that claim is only meaningful if both sides run the same chapter logic. A second
// copy of this function would make the gate test the copy, not the product.
//
// Semantics (unchanged from the serial path, which is the reference):
//   * at most 100 entries, taken in TOC order (official MAX_TOC_EXPORT)
//   * then a STABLE sort by page, so equal-page entries keep TOC order
//   * same-page duplicates collapse (a sub-entry aliasing its parent's heading)
//   * each chapter ends one page before the next; the last ends at the final page
//   * no candidates at all → fall back to per-spine chapters (books without a usable TOC)
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "xtch_writer.h"

namespace ko {

inline std::vector<XtchChapter> buildChapters(
    const std::vector<ChapterCandidate>& candIn,
    const std::vector<XtchChapter>& fallback,
    uint32_t totalPages) {
  std::vector<XtchChapter> out;
  const uint32_t lastPage = totalPages > 0 ? totalPages - 1 : 0;
  std::vector<ChapterCandidate> cand = candIn;
  if (cand.size() > 100) cand.resize(100);
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
      ch.name = uniq[i].title.empty() ? ("Chapter " + std::to_string(i + 1)) : uniq[i].title;
      ch.startPage = static_cast<uint16_t>(uniq[i].page);
      ch.endPage = static_cast<uint16_t>((i + 1 < uniq.size()) ? uniq[i + 1].page - 1 : lastPage);
      if (ch.endPage < ch.startPage) ch.endPage = ch.startPage;
      if (ch.startPage <= lastPage) out.push_back(ch);
    }
  } else if (!fallback.empty()) {
    out = fallback;
  }
  return out;
}

}  // namespace ko
