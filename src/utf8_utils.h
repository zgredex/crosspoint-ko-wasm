#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

namespace ko {

// Return the largest prefix no longer than maxBytes that consists entirely of
// complete, canonical UTF-8 scalar values. Invalid input stops at the last
// valid scalar so no caller can serialize or expose a malformed suffix.
inline size_t utf8SafePrefixLength(const char* data, size_t size, size_t maxBytes) {
  if (!data) return 0;
  const size_t limit = std::min(size, maxBytes);
  size_t i = 0;
  while (i < limit) {
    const uint8_t lead = static_cast<uint8_t>(data[i]);
    size_t n = 0;
    uint32_t cp = 0;
    if (lead <= 0x7f) {
      n = 1;
      cp = lead;
    } else if (lead >= 0xc2 && lead <= 0xdf) {
      n = 2;
      cp = lead & 0x1fu;
    } else if (lead >= 0xe0 && lead <= 0xef) {
      n = 3;
      cp = lead & 0x0fu;
    } else if (lead >= 0xf0 && lead <= 0xf4) {
      n = 4;
      cp = lead & 0x07u;
    } else {
      break;
    }
    if (n > limit - i) break;
    bool valid = true;
    for (size_t j = 1; j < n; ++j) {
      const uint8_t c = static_cast<uint8_t>(data[i + j]);
      if ((c & 0xc0u) != 0x80u) {
        valid = false;
        break;
      }
      cp = (cp << 6) | (c & 0x3fu);
    }
    if (!valid || (n == 2 && cp < 0x80u) || (n == 3 && cp < 0x800u) ||
        (n == 4 && cp < 0x10000u) || (cp >= 0xd800u && cp <= 0xdfffu) ||
        cp > 0x10ffffu) {
      break;
    }
    i += n;
  }
  return i;
}

inline size_t utf8SafePrefixLength(const std::string& value, size_t maxBytes) {
  return utf8SafePrefixLength(value.data(), value.size(), maxBytes);
}

}  // namespace ko
