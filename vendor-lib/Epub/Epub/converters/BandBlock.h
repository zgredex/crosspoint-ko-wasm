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
