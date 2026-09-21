#pragma once

#include <algorithm>
#include <string>

#include "utf8_utils.h"

namespace ko {

// Normalize text already supplied by the EPUB navigation document. This does
// not inspect XHTML content: it only collapses ASCII whitespace and refuses to
// cut a malformed or overlong UTF-8 title in the middle of a code point.
inline std::string normalizeChapterTitle(const std::string& input) {
  std::string out;
  out.reserve(std::min<size_t>(input.size(), 1024));
  bool space = true;
  for (size_t i = 0; i < input.size();) {
    const unsigned char c = static_cast<unsigned char>(input[i]);
    const bool whitespace = c == ' ' || c == '\t' || c == '\r' || c == '\n';
    if (whitespace) {
      if (!space && !out.empty()) out.push_back(' ');
      space = true;
      ++i;
    } else if (c >= 0x20 && c < 0x80) {
      if (out.size() + 1 > 1024) break;
      out.push_back(static_cast<char>(c));
      space = false;
      ++i;
    } else if (c >= 0x80) {
      size_t width = 0;
      if (c >= 0xc2 && c <= 0xdf) width = 2;
      else if (c >= 0xe0 && c <= 0xef) width = 3;
      else if (c >= 0xf0 && c <= 0xf4) width = 4;
      if (width == 0 || width > input.size() - i || out.size() + width > 1024 ||
          utf8SafePrefixLength(input.data() + i, width, width) != width) {
        break;
      }
      out.append(input, i, width);
      space = false;
      i += width;
    } else {
      ++i;
    }
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

}  // namespace ko
