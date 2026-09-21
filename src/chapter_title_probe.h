#pragma once

#include <Print.h>
#include <expat.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

#include <XmlParserUtils.h>
#include <htmlEntities.h>
#include "utf8_utils.h"

namespace ko {

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

inline bool isGenericChapterTitle(const std::string& input) {
  const std::string clean = normalizeChapterTitle(input);
  if (clean.empty()) return true;
  std::string lower = clean;
  for (char& byte : lower) {
    unsigned char c = static_cast<unsigned char>(byte);
    if (c >= 'A' && c <= 'Z') byte = static_cast<char>(c + ('a' - 'A'));
  }
  if (lower == "untitled" || lower == "contents" || lower == "table of contents" ||
      lower == "front matter" || lower == "목차" || lower == "본문") {
    return true;
  }

  const auto numberedRemainder = [](const std::string& value) {
    std::string token;
    for (unsigned char c : value) {
      if (c == ' ' || c == '\t' || c == '.' || c == '-' || c == '_' || c == ':' ||
          c == '(' || c == ')' || c == '[' || c == ']') continue;
      if ((c >= '0' && c <= '9') || c == 'i' || c == 'v' || c == 'x' || c == 'l' ||
          c == 'c' || c == 'd' || c == 'm') {
        token.push_back(static_cast<char>(c));
        continue;
      }
      return false;
    }
    return !token.empty();
  };
  const char* prefixes[] = {"section", "chapter", "part", "unit", "text", "content"};
  for (const char* prefix : prefixes) {
    const size_t n = std::strlen(prefix);
    if (lower.compare(0, n, prefix) == 0 &&
        (lower.size() == n || numberedRemainder(lower.substr(n)))) {
      return true;
    }
  }

  if (lower.compare(0, std::strlen("섹션"), "섹션") == 0 &&
      numberedRemainder(lower.substr(std::strlen("섹션")))) return true;
  if (lower.compare(0, std::strlen("장"), "장") == 0 &&
      numberedRemainder(lower.substr(std::strlen("장")))) return true;
  if (lower.compare(0, std::strlen("제"), "제") == 0) {
    const size_t suffix = lower.find("장", std::strlen("제"));
    if (suffix != std::string::npos && suffix + std::strlen("장") == lower.size()) {
      const std::string number = lower.substr(std::strlen("제"), suffix - std::strlen("제"));
      if (!number.empty() && std::all_of(number.begin(), number.end(), [](unsigned char c) {
            return c >= '0' && c <= '9';
          })) return true;
    }
  }

  std::string stem;
  stem.reserve(lower.size());
  for (unsigned char c : lower) {
    if (c >= 'a' && c <= 'z') stem.push_back(static_cast<char>(c));
  }
  return stem == "section" || stem == "chapter" || stem == "part" || stem == "unit" ||
         stem == "untitled" || stem == "text" || stem == "content";
}

// Bounded, streaming probe for the first visible XHTML heading.  It is used
// only to label the chapter picker before a spine has been paginated; complete
// export chapter mapping comes from ChapterHtmlSlimParser's full parse.
class ChapterTitleProbe final : public Print {
 public:
  static constexpr size_t kMaxProbeBytes = 256u * 1024u;

  explicit ChapterTitleProbe(size_t maxBytes = kMaxProbeBytes)
      : maxBytes_(std::min(maxBytes, kMaxProbeBytes)) {
    parser_ = XML_ParserCreate(nullptr);
    if (parser_) {
      XML_SetUserData(parser_, this);
      XML_SetElementHandler(parser_, startElement, endElement);
      XML_SetCharacterDataHandler(parser_, characterData);
      XML_SetDefaultHandlerExpand(parser_, defaultHandlerExpand);
    }
  }
  ~ChapterTitleProbe() override { destroyXmlParser(parser_); }

  size_t write(uint8_t byte) override { return write(&byte, 1); }
  size_t write(const uint8_t* data, size_t size) override {
    if (done_ || consumed_ >= maxBytes_) return 0;
    const size_t take = std::min(size, maxBytes_ - consumed_);
    // Charge every byte delivered by the inflater, including the chunk that
    // exposes malformed XML (or arrives after parser allocation failed). The
    // per-book budget is a decompression-work ceiling, not merely a count of
    // bytes Expat accepted successfully.
    consumed_ += take;
    if (!parser_) {
      done_ = true;
      return 0;
    }
    if (XML_Parse(parser_, reinterpret_cast<const char*>(data), static_cast<int>(take), false) ==
        XML_STATUS_ERROR) {
      destroyXmlParser(parser_);
      parser_ = nullptr;
      done_ = true;
      return 0;
    }
    // A short write asks ZipFile's explicitly-enabled early-stop path to stop
    // inflating without pretending the unconsumed member was CRC-verified.
    return done_ || take != size || consumed_ == maxBytes_ ? 0 : size;
  }

  std::string bestTitle() const {
    const std::string heading = normalizeChapterTitle(heading_);
    return heading.empty() ? normalizeChapterTitle(documentTitle_) : heading;
  }
  std::string headingTitle() const { return normalizeChapterTitle(heading_); }
  std::string documentTitle() const { return normalizeChapterTitle(documentTitle_); }
  size_t consumedBytes() const { return consumed_; }

 private:
  static bool headingTag(const char* name) {
    return name && name[0] != '\0' && name[1] >= '1' && name[1] <= '6' && name[2] == '\0' &&
           (name[0] == 'h' || name[0] == 'H');
  }
  static bool hiddenElement(const XML_Char* name, const XML_Char** atts) {
    if (strcasecmp(name, "script") == 0 || strcasecmp(name, "style") == 0 ||
        strcasecmp(name, "template") == 0 || strcasecmp(name, "noscript") == 0) return true;
    if (!atts) return false;
    for (int i = 0; atts[i]; i += 2) {
      if (strcasecmp(atts[i], "hidden") == 0) return true;
      if (strcasecmp(atts[i], "aria-hidden") == 0 && strcasecmp(atts[i + 1], "true") == 0) return true;
      if (strcasecmp(atts[i], "style") == 0) {
        std::string style = atts[i + 1];
        style.erase(std::remove_if(style.begin(), style.end(), [](unsigned char c) {
          return c == ' ' || c == '\t' || c == '\r' || c == '\n';
        }), style.end());
        for (char& c : style) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
        if (style.find("display:none") != std::string::npos ||
            style.find("visibility:hidden") != std::string::npos) return true;
      }
    }
    return false;
  }
  static void XMLCALL startElement(void* opaque, const XML_Char* name, const XML_Char** atts) {
    auto* self = static_cast<ChapterTitleProbe*>(opaque);
    if (self->done_) return;
    self->depth_++;
    if (strcasecmp(name, "body") == 0) self->insideBody_ = true;
    if (!self->insideBody_ && strcasecmp(name, "title") == 0) self->titleDepth_ = self->depth_;
    if (self->insideBody_ && self->hiddenDepth_ < 0 && hiddenElement(name, atts)) {
      self->hiddenDepth_ = self->depth_;
    }
    if (self->insideBody_ && self->hiddenDepth_ < 0 && self->headingDepth_ < 0 && headingTag(name)) {
      self->headingDepth_ = self->depth_;
      self->headingText_.clear();
    }
  }
  static void XMLCALL characterData(void* opaque, const XML_Char* text, int len) {
    auto* self = static_cast<ChapterTitleProbe*>(opaque);
    if (self->done_) return;
    std::string* target = nullptr;
    if (self->headingDepth_ >= 0 && self->hiddenDepth_ < 0) target = &self->headingText_;
    else if (self->titleDepth_ >= 0) target = &self->documentTitle_;
    if (!target || target->size() >= 4096) return;
    target->append(text, std::min<size_t>(static_cast<size_t>(len), 4096 - target->size()));
  }
  static void XMLCALL endElement(void* opaque, const XML_Char* name) {
    auto* self = static_cast<ChapterTitleProbe*>(opaque);
    if (self->done_) return;
    if (self->headingDepth_ == self->depth_ && headingTag(name)) {
      const std::string candidate = normalizeChapterTitle(self->headingText_);
      if (!candidate.empty()) {
        if (self->heading_.empty()) self->heading_ = candidate;
        if (!isGenericChapterTitle(candidate)) {
          self->heading_ = candidate;
          self->done_ = true;
        }
      }
      self->headingDepth_ = -1;
    }
    if (self->titleDepth_ == self->depth_ && strcasecmp(name, "title") == 0) {
      self->titleDepth_ = -1;
    }
    if (self->hiddenDepth_ == self->depth_) self->hiddenDepth_ = -1;
    if (strcasecmp(name, "body") == 0) self->insideBody_ = false;
    self->depth_--;
  }
  static void XMLCALL defaultHandlerExpand(void* opaque, const XML_Char* text, int len) {
    if (len >= 3 && text[0] == '&' && text[len - 1] == ';') {
      const char* value = lookupHtmlEntity(text, static_cast<size_t>(len));
      if (value) {
        characterData(opaque, value, static_cast<int>(std::strlen(value)));
        return;
      }
      characterData(opaque, text, len);
    }
  }

  XML_Parser parser_ = nullptr;
  size_t maxBytes_ = kMaxProbeBytes;
  size_t consumed_ = 0;
  int depth_ = 0;
  int titleDepth_ = -1;
  int headingDepth_ = -1;
  int hiddenDepth_ = -1;
  bool insideBody_ = false;
  bool done_ = false;
  std::string documentTitle_;
  std::string heading_;
  std::string headingText_;
};

}  // namespace ko
