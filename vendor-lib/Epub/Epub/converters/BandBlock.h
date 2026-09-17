#pragma once

#include <cstdint>

// One decoded band handed to a converter's raster callback.
//
// This is deliberately shaped like the MCU-decoder draw callback it replaces: the
// sampling, cropping, dithering and BMP-writing code in the converters is written
// against these fields, and feeding a whole decoded frame as a single band
// (x = 0, y = 0 / band start, iWidthUsed = iWidth) makes the block-relative maths the
// identity instead of rewriting it. That is what keeps the destination box pinned.
struct BandBlock {
  void* pUser = nullptr;
  int x = 0;           // left edge of the band in source pixels
  int y = 0;           // top edge of the band in source pixels
  int iWidth = 0;      // stride of pPixels, in pixels
  int iWidthUsed = 0;  // number of valid columns
  int iHeight = 0;     // number of rows in the band
  uint16_t* pPixels = nullptr;  // 8-bit greyscale, densely packed, cast for the ABI
};

// ---------------------------------------------------------------------------
// PNGdec-compatible scanline block and pixel-type tags.
//
// PNGdec has been removed from the build. These definitions keep the PNG framebuffer
// converter's callback signature and its comparisons byte-for-byte identical, so removing
// the library is provably behaviour-neutral. Field names match PNGdec's, which is why the
// callback body needs no change at all.
// ---------------------------------------------------------------------------
inline constexpr int PNG_SUCCESS = 0;  // PNGdec's success code, kept for the call sites

enum PngPixelType {
  PNG_PIXEL_GRAYSCALE = 0,
  PNG_PIXEL_GRAY_ALPHA = 1,
  PNG_PIXEL_TRUECOLOR = 2,
  PNG_PIXEL_TRUECOLOR_ALPHA = 3,
  PNG_PIXEL_INDEXED = 4,
};

struct PNGDRAW {
  void* pUser = nullptr;
  int y = 0;                 // source row index
  int iBpp = 8;
  int iHasAlpha = 0;
  int iPixelType = PNG_PIXEL_GRAYSCALE;
  void* pPalette = nullptr;  // always null: palette is expanded to RGB, then to grey
  uint8_t* pPixels = nullptr;
};
