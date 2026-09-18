// layout_manifest.h — a machine-comparable dump of the layout the engine decided.
//
// WHY THIS EXISTS: comparing only final containers tells you THAT two builds differ,
// never WHERE. The conformance gate against CrossPoint-KO needs the layout decisions
// themselves — per page, per line, per word, at the exact x the renderer will use — so
// a divergence points at the function that made it instead of at a byte offset.
//
// Coordinates are PAGE-LOCAL, i.e. exactly the values the engine stores in the page:
//   line.y  = PageLine::yPos          (0 = top of the text viewport)
//   word.x  = PageLine::xPos + TextBlock::wordXpos(i)   (0 = left of the text viewport)
// The exporters add the margins when they render (Page::render(..., xOffset, yOffset)),
// so the manifest header carries the margins and the viewport instead of baking them in.
//
// TEXT IS UTF-8, escaped for JSON. Nothing else about the words is normalised — no
// trimming, no NFC, no space synthesis — so a spacing divergence cannot hide here.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <Epub/Page.h>

namespace ko {

struct ManifestWord {
  std::string text;
  int x = 0;             // page-local x of the word's left edge
  uint8_t style = 0;     // EpdFontFamily::Style
  uint8_t focusBoundary = 0;  // 0 = no bold split, else first N bytes bold
};

struct ManifestLine {
  int x = 0;
  int y = 0;
  std::vector<ManifestWord> words;
  std::vector<std::string> ruby;
  bool hasRuby = false;
};

struct ManifestImage {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
};

struct ManifestRule {
  int x = 0;
  int y = 0;
};

struct ManifestPage {
  int spine = 0;
  int page = 0;
  uint32_t visibleTextOffset = 0;
  std::vector<ManifestLine> lines;
  std::vector<ManifestImage> images;
  std::vector<ManifestRule> rules;
};

// Snapshot one loaded page. Must be called while the page is alive: everything is
// copied out here so the caller can drop the page and serialize later.
inline ManifestPage probePage(const Page& page, int spine, int pageIndex) {
  ManifestPage out;
  out.spine = spine;
  out.page = pageIndex;
  out.visibleTextOffset = page.visibleTextOffset;
  for (const auto& element : page.elements) {
    switch (element->getTag()) {
      case TAG_PageLine: {
        const auto& line = static_cast<const PageLine&>(*element);
        const auto& block = line.getBlock();
        if (!block) break;
        ManifestLine ml;
        ml.x = line.xPos;
        ml.y = line.yPos;
        const uint16_t count = block->wordCount();
        for (uint16_t i = 0; i < count; i++) {
          ManifestWord mw;
          mw.text.assign(block->wordText(i), block->wordTextLen(i));
          mw.x = static_cast<int>(line.xPos) + block->wordXpos(i);
          mw.style = static_cast<uint8_t>(block->wordStyle(i));
          mw.focusBoundary = block->focusBoundary(i);
          ml.words.push_back(std::move(mw));
        }
        if (block->hasRuby()) {
          ml.hasRuby = true;
          ml.ruby = block->getRubyTexts();
        }
        out.lines.push_back(std::move(ml));
        break;
      }
      case TAG_PageImage: {
        const auto& img = static_cast<const PageImage&>(*element);
        ManifestImage mi;
        mi.x = img.xPos;
        mi.y = img.yPos;
        mi.w = img.getImageBlock().getWidth();
        mi.h = img.getImageBlock().getHeight();
        out.images.push_back(mi);
        break;
      }
      case TAG_PageHorizontalRule:
        out.rules.push_back(ManifestRule{element->xPos, element->yPos});
        break;
    }
  }
  return out;
}

// --- JSON serialization -----------------------------------------------------
// Minimally correct for JSON: escapes what the spec requires, leaves everything
// else (including all non-ASCII) as raw UTF-8 bytes so two runs of the same engine
// produce identical text. \u escapes are deliberately NOT used for non-ASCII: the
// bytes are valid UTF-8 and re-encoding them would only add a way to differ.
inline void jsonString(std::string& out, const std::string& s) {
  out += '"';
  for (const unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  out += '"';
}

struct ManifestHeader {
  std::string oracleRepo;      // e.g. crosspoint-reader-ko/crosspoint-reader-ko
  std::string oracleCommit;    // 40-hex; empty when the caller does not know it
  std::string oracleBranch;
  int screenWidth = 0;
  int screenHeight = 0;
  int marginTop = 0;
  int marginRight = 0;
  int marginBottom = 0;
  int marginLeft = 0;
  int viewportWidth = 0;
  int viewportHeight = 0;
  int fontId = 0;
  float lineCompression = 0;
  bool characterWrap = false;
  bool hyphenation = false;
  bool embeddedStyle = false;
  bool paragraphIndent = false;
  bool extraParagraphSpacing = false;
  int paragraphAlignment = 0;
  int imageRendering = 0;
  bool textAa = false;
};

inline std::string serializeLayoutManifest(const ManifestHeader& h, const std::vector<ManifestPage>& pages) {
  std::string out;
  out.reserve(1 << 20);
  out += "{\n  \"oracle\": {";
  out += "\"repo\": ";
  jsonString(out, h.oracleRepo);
  out += ", \"branch\": ";
  jsonString(out, h.oracleBranch);
  out += ", \"commit\": ";
  jsonString(out, h.oracleCommit);
  out += "},\n  \"screen\": [";
  out += std::to_string(h.screenWidth) + ", " + std::to_string(h.screenHeight) + "],\n";
  out += "  \"margins\": [";
  out += std::to_string(h.marginTop) + ", " + std::to_string(h.marginRight) + ", " +
         std::to_string(h.marginBottom) + ", " + std::to_string(h.marginLeft) + "],\n";
  out += "  \"viewport\": [" + std::to_string(h.viewportWidth) + ", " + std::to_string(h.viewportHeight) + "],\n";
  out += "  \"font\": " + std::to_string(h.fontId) + ",\n";
  out += "  \"spec\": {\"lineCompression\": ";
  {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.4f", h.lineCompression);
    out += buf;
  }
  out += ", \"characterWrap\": " + std::string(h.characterWrap ? "true" : "false");
  out += ", \"hyphenation\": " + std::string(h.hyphenation ? "true" : "false");
  out += ", \"embeddedStyle\": " + std::string(h.embeddedStyle ? "true" : "false");
  out += ", \"paragraphIndent\": " + std::string(h.paragraphIndent ? "true" : "false");
  out += ", \"extraParagraphSpacing\": " + std::string(h.extraParagraphSpacing ? "true" : "false");
  out += ", \"paragraphAlignment\": " + std::to_string(h.paragraphAlignment);
  out += ", \"imageRendering\": " + std::to_string(h.imageRendering);
  out += ", \"textAa\": " + std::string(h.textAa ? "true" : "false");
  out += "},\n  \"pages\": [\n";
  for (size_t p = 0; p < pages.size(); p++) {
    const ManifestPage& pg = pages[p];
    out += "    {\"spine\": " + std::to_string(pg.spine) + ", \"page\": " + std::to_string(pg.page) +
           ", \"visibleTextOffset\": " + std::to_string(pg.visibleTextOffset) + ", \"lines\": [";
    for (size_t l = 0; l < pg.lines.size(); l++) {
      const ManifestLine& ln = pg.lines[l];
      out += (l ? ", " : "") + std::string("{\"x\": ") + std::to_string(ln.x) + ", \"y\": " + std::to_string(ln.y) +
             ", \"w\": [";
      for (size_t w = 0; w < ln.words.size(); w++) {
        const ManifestWord& mw = ln.words[w];
        out += (w ? ", " : "") + std::string("{\"x\": ") + std::to_string(mw.x) + ", \"s\": " +
               std::to_string(mw.style) + ", \"t\": ";
        jsonString(out, mw.text);
        if (mw.focusBoundary) out += ", \"fb\": " + std::to_string(mw.focusBoundary);
        out += "}";
      }
      out += "]";
      if (ln.hasRuby) {
        out += ", \"ruby\": [";
        for (size_t r = 0; r < ln.ruby.size(); r++) {
          out += (r ? ", " : "");
          jsonString(out, ln.ruby[r]);
        }
        out += "]";
      }
      out += "}";
    }
    out += "], \"images\": [";
    for (size_t i = 0; i < pg.images.size(); i++) {
      const ManifestImage& im = pg.images[i];
      out += (i ? ", " : "") + std::string("{\"x\": ") + std::to_string(im.x) + ", \"y\": " + std::to_string(im.y) +
             ", \"w\": " + std::to_string(im.w) + ", \"h\": " + std::to_string(im.h) + "}";
    }
    out += "], \"rules\": [";
    for (size_t i = 0; i < pg.rules.size(); i++) {
      out += (i ? ", " : "") + std::string("{\"x\": ") + std::to_string(pg.rules[i].x) +
             ", \"y\": " + std::to_string(pg.rules[i].y) + "}";
    }
    out += "]}";
    out += (p + 1 < pages.size()) ? ",\n" : "\n";
  }
  out += "  ]\n}\n";
  return out;
}

inline bool writeFile(const std::string& path, const std::string& bytes) {
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) return false;
  const bool ok = fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
  fclose(f);
  return ok;
}

}  // namespace ko
