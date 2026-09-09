#include "EpdFontFamily.h"

const EpdFont* EpdFontFamily::getFont(const Style style) const {
  // Extract font style bits; render-time overlay bits do not affect font selection.
  const bool hasBold = (style & BOLD) != 0;
  const bool hasItalic = (style & ITALIC) != 0;

  if (hasBold && hasItalic) {
    if (boldItalic) return boldItalic;
    if (bold) return bold;
    if (italic) return italic;
  } else if (hasBold && bold) {
    return bold;
  } else if (hasItalic && italic) {
    return italic;
  }

  return regular;
}

void EpdFontFamily::getTextDimensions(const char* string, int* w, int* h, const Style style) const {
  getFont(style)->getTextDimensions(string, w, h);
}

const EpdFontData* EpdFontFamily::getData(const Style style) const { return getFont(style)->data; }

const EpdGlyph* EpdFontFamily::getGlyph(const uint32_t cp, const Style style) const {
  return getFont(style)->getGlyph(cp);
}

bool EpdFontFamily::hasPrintableChars(const char* string, const Style style) const {
  const EpdFont* font = getFont(style);
  if (!font || !string) return false;
  const uint8_t* p = reinterpret_cast<const uint8_t*>(string);
  while (*p) {
    uint32_t cp;
    if (*p < 0x80) {
      cp = *p++;
    } else if ((*p & 0xE0) == 0xC0) {
      cp = (*p++ & 0x1F) << 6;
      if (*p) cp |= (*p++ & 0x3F);
    } else if ((*p & 0xF0) == 0xE0) {
      cp = (*p++ & 0x0F) << 12;
      if (*p) cp |= (*p++ & 0x3F) << 6;
      if (*p) cp |= (*p++ & 0x3F);
    } else {
      cp = (*p++ & 0x07) << 18;
      if (*p) cp |= (*p++ & 0x3F) << 12;
      if (*p) cp |= (*p++ & 0x3F) << 6;
      if (*p) cp |= (*p++ & 0x3F);
    }
    if (font->getGlyph(cp)) return true;
  }
  return false;
}

int8_t EpdFontFamily::getKerning(const uint32_t leftCp, const uint32_t rightCp, const Style style) const {
  return getFont(style)->getKerning(leftCp, rightCp);
}

uint32_t EpdFontFamily::applyLigatures(const uint32_t cp, const char*& text, const Style style) const {
  return getFont(style)->applyLigatures(cp, text);
}
