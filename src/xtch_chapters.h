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
//   * EPUB navigation entries are authoritative and take slots first, in TOC order
//   * inferred visible headings fill only the remaining slots, in document order
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

inline constexpr size_t MAX_EXPORTED_CHAPTERS = 100;

// Keep collection bounded even when candidates arrive interleaved by spine
// (inferred headings from an early spine can precede authoritative navigation
// entries from a later one). The final format uses at most 100 entries, so the
// first 100 authoritative candidates plus the first 100 distinct inferred
// pages are sufficient: at most N inferred pages can collide with N retained
// authoritative pages, leaving enough inferred pages to fill every free slot.
inline bool retainChapterCandidate(std::vector<ChapterCandidate>& candidates,
                                   ChapterCandidate candidate) {
  size_t sourceCount = 0;
  for (const auto& existing : candidates) {
    if (existing.inferred == candidate.inferred) sourceCount++;
    if (candidate.inferred && existing.page == candidate.page) return false;
  }
  if (sourceCount >= MAX_EXPORTED_CHAPTERS) return false;
  candidates.push_back(std::move(candidate));
  return true;
}

inline std::vector<XtchChapter> buildChapters(
    const std::vector<ChapterCandidate>& candIn,
    const std::vector<XtchChapter>& fallback,
    uint32_t totalPages) {
  std::vector<XtchChapter> out;
  const uint32_t lastPage = totalPages > 0 ? totalPages - 1 : 0;
  std::vector<ChapterCandidate> cand;
  cand.reserve(std::min(MAX_EXPORTED_CHAPTERS, candIn.size()));
  for (const bool inferred : {false, true}) {
    for (const auto& c : candIn) {
      if (c.inferred != inferred) continue;
      if (cand.size() == MAX_EXPORTED_CHAPTERS) break;
      // An inferred heading on a page already named by navigation adds no
      // device-visible boundary and must not consume one of the finite slots.
      if (inferred && std::any_of(cand.begin(), cand.end(), [&c](const ChapterCandidate& existing) {
            return existing.page == c.page;
          })) continue;
      cand.push_back(c);
    }
    if (cand.size() == MAX_EXPORTED_CHAPTERS) break;
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
