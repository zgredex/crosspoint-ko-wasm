#include "JpegToBmpConverter.h"

#include <HalDisplay.h>
#include <HalStorage.h>
#include "../Epub/Epub/converters/BandBlock.h"
#include <csetjmp>
#include <jpeglib.h>
#include <Logging.h>
#include <Memory.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdio>
#include <algorithm>
#include <cstring>
#include <vector>

#include "BitmapHelpers.h"
// Device-era cooperative yield: the decoder used to run long enough on an ESP32 to trip
// the task watchdog, so the MCU path yielded to the idle task between blocks. On host and
// wasm there is nothing to yield to, so this is deliberately a no-op rather than a
// reimplementation of a scheduler we do not have.
void yieldToIdle() {}

// ============================================================================
// IMAGE PROCESSING OPTIONS - Toggle these to test different configurations
// ============================================================================
constexpr bool USE_8BIT_OUTPUT = false;  // true: 8-bit grayscale (no quantization), false: 2-bit (4 levels)
// Dithering method selection (only one should be true, or all false for simple quantization):
constexpr bool USE_ATKINSON = true;          // Atkinson dithering (cleaner than F-S, less error diffusion)
constexpr bool USE_FLOYD_STEINBERG = false;  // Floyd-Steinberg error diffusion (can cause "worm" artifacts)
constexpr bool USE_NOISE_DITHERING = false;  // Hash-based noise dithering (good for downsampling)
// Pre-resize to target display size (CRITICAL: avoids dithering artifacts from post-downsampling)
constexpr bool USE_PRESCALE = true;  // true: scale image to target size before dithering
// ============================================================================

inline void write16(Print& out, const uint16_t value) {
  out.write(value & 0xFF);
  out.write((value >> 8) & 0xFF);
}

inline void write32(Print& out, const uint32_t value) {
  out.write(value & 0xFF);
  out.write((value >> 8) & 0xFF);
  out.write((value >> 16) & 0xFF);
  out.write((value >> 24) & 0xFF);
}

inline void write32Signed(Print& out, const int32_t value) {
  out.write(value & 0xFF);
  out.write((value >> 8) & 0xFF);
  out.write((value >> 16) & 0xFF);
  out.write((value >> 24) & 0xFF);
}

// Helper function: Write BMP header with 8-bit grayscale (256 levels)
void writeBmpHeader8bit(Print& bmpOut, const int width, const int height) {
  // Calculate row padding (each row must be multiple of 4 bytes)
  const int bytesPerRow = (width + 3) / 4 * 4;  // 8 bits per pixel, padded
  const int imageSize = bytesPerRow * height;
  const uint32_t paletteSize = 256 * 4;  // 256 colors * 4 bytes (BGRA)
  const uint32_t fileSize = 14 + 40 + paletteSize + imageSize;

  // BMP File Header (14 bytes)
  bmpOut.write('B');
  bmpOut.write('M');
  write32(bmpOut, fileSize);
  write32(bmpOut, 0);                      // Reserved
  write32(bmpOut, 14 + 40 + paletteSize);  // Offset to pixel data

  // DIB Header (BITMAPINFOHEADER - 40 bytes)
  write32(bmpOut, 40);
  write32Signed(bmpOut, width);
  write32Signed(bmpOut, -height);  // Negative height = top-down bitmap
  write16(bmpOut, 1);              // Color planes
  write16(bmpOut, 8);              // Bits per pixel (8 bits)
  write32(bmpOut, 0);              // BI_RGB (no compression)
  write32(bmpOut, imageSize);
  write32(bmpOut, 2835);  // xPixelsPerMeter (72 DPI)
  write32(bmpOut, 2835);  // yPixelsPerMeter (72 DPI)
  write32(bmpOut, 256);   // colorsUsed
  write32(bmpOut, 256);   // colorsImportant

  // Color Palette (256 grayscale entries x 4 bytes = 1024 bytes)
  for (int i = 0; i < 256; i++) {
    bmpOut.write(static_cast<uint8_t>(i));  // Blue
    bmpOut.write(static_cast<uint8_t>(i));  // Green
    bmpOut.write(static_cast<uint8_t>(i));  // Red
    bmpOut.write(static_cast<uint8_t>(0));  // Reserved
  }
}

// Helper function: Write BMP header with 1-bit color depth (black and white)
static void writeBmpHeader1bit(Print& bmpOut, const int width, const int height) {
  // Calculate row padding (each row must be multiple of 4 bytes)
  const int bytesPerRow = (width + 31) / 32 * 4;  // 1 bit per pixel, round up to 4-byte boundary
  const int imageSize = bytesPerRow * height;
  const uint32_t fileSize = 62 + imageSize;  // 14 (file header) + 40 (DIB header) + 8 (palette) + image

  // BMP File Header (14 bytes)
  bmpOut.write('B');
  bmpOut.write('M');
  write32(bmpOut, fileSize);  // File size
  write32(bmpOut, 0);         // Reserved
  write32(bmpOut, 62);        // Offset to pixel data (14 + 40 + 8)

  // DIB Header (BITMAPINFOHEADER - 40 bytes)
  write32(bmpOut, 40);
  write32Signed(bmpOut, width);
  write32Signed(bmpOut, -height);  // Negative height = top-down bitmap
  write16(bmpOut, 1);              // Color planes
  write16(bmpOut, 1);              // Bits per pixel (1 bit)
  write32(bmpOut, 0);              // BI_RGB (no compression)
  write32(bmpOut, imageSize);
  write32(bmpOut, 2835);  // xPixelsPerMeter (72 DPI)
  write32(bmpOut, 2835);  // yPixelsPerMeter (72 DPI)
  write32(bmpOut, 2);     // colorsUsed
  write32(bmpOut, 2);     // colorsImportant

  // Color Palette (2 colors x 4 bytes = 8 bytes)
  // Format: Blue, Green, Red, Reserved (BGRA)
  // Note: In 1-bit BMP, palette index 0 = black, 1 = white
  uint8_t palette[8] = {
      0x00, 0x00, 0x00, 0x00,  // Color 0: Black
      0xFF, 0xFF, 0xFF, 0x00   // Color 1: White
  };
  for (const uint8_t i : palette) {
    bmpOut.write(i);
  }
}

// Helper function: Write BMP header with 2-bit color depth
static void writeBmpHeader2bit(Print& bmpOut, const int width, const int height) {
  // Calculate row padding (each row must be multiple of 4 bytes)
  const int bytesPerRow = (width * 2 + 31) / 32 * 4;  // 2 bits per pixel, round up
  const int imageSize = bytesPerRow * height;
  const uint32_t fileSize = 70 + imageSize;  // 14 (file header) + 40 (DIB header) + 16 (palette) + image

  // BMP File Header (14 bytes)
  bmpOut.write('B');
  bmpOut.write('M');
  write32(bmpOut, fileSize);  // File size
  write32(bmpOut, 0);         // Reserved
  write32(bmpOut, 70);        // Offset to pixel data

  // DIB Header (BITMAPINFOHEADER - 40 bytes)
  write32(bmpOut, 40);
  write32Signed(bmpOut, width);
  write32Signed(bmpOut, -height);  // Negative height = top-down bitmap
  write16(bmpOut, 1);              // Color planes
  write16(bmpOut, 2);              // Bits per pixel (2 bits)
  write32(bmpOut, 0);              // BI_RGB (no compression)
  write32(bmpOut, imageSize);
  write32(bmpOut, 2835);  // xPixelsPerMeter (72 DPI)
  write32(bmpOut, 2835);  // yPixelsPerMeter (72 DPI)
  write32(bmpOut, 4);     // colorsUsed
  write32(bmpOut, 4);     // colorsImportant

  // Color Palette (4 colors x 4 bytes = 16 bytes)
  // Format: Blue, Green, Red, Reserved (BGRA)
  uint8_t palette[16] = {
      0x00, 0x00, 0x00, 0x00,  // Color 0: Black
      0x55, 0x55, 0x55, 0x00,  // Color 1: Dark gray (85)
      0xAA, 0xAA, 0xAA, 0x00,  // Color 2: Light gray (170)
      0xFF, 0xFF, 0xFF, 0x00   // Color 3: White
  };
  for (const uint8_t i : palette) {
    bmpOut.write(i);
  }
}

namespace {

// libjpeg's default error_exit is exit(). A corrupt cover must fail the conversion,
// not the process (and in wasm, not the whole app).
struct BmpJpegError {
  jpeg_error_mgr pub;
  jmp_buf escape;
  char message[JMSG_LENGTH_MAX]{};
};

void bmpJpegErrorExit(j_common_ptr cinfo) {
  BmpJpegError* self = reinterpret_cast<BmpJpegError*>(cinfo->err);
  (*cinfo->err->format_message)(cinfo, self->message);
  longjmp(self->escape, 1);
}


// Max MCU height supported by any JPEG (4:2:0 chroma = 16 rows, 4:4:4 = 8 rows)
constexpr int MAX_MCU_HEIGHT = 16;
constexpr size_t JPEG_DECODER_SIZE = 20 * 1024;
constexpr size_t MIN_FREE_HEAP = JPEG_DECODER_SIZE + 32 * 1024;
constexpr uint32_t FP_ONE = 1UL << 16;


// Context passed to the JPEGDEC draw callback via setUserPointer()
struct BmpConvertCtx {
  Print* bmpOut;
  int srcWidth;
  int srcHeight;
  int outWidth;
  int outHeight;
  bool oneBit;
  int bytesPerRow;
  bool needsScaling;
  uint32_t scaleX_fp;  // source pixels per output pixel, 16.16 fixed-point
  uint32_t scaleY_fp;
  bool smoothUpscale;
  uint32_t smoothScaleX_fp;
  uint32_t smoothScaleY_fp;

  // Accumulates one MCU row (up to MAX_MCU_HEIGHT source rows × srcWidth pixels)
  // Filled column-by-column as JPEGDEC callbacks arrive for the same MCU row
  std::unique_ptr<uint8_t[]> mcuBuf;

  // Y-axis area averaging accumulators (needsScaling only)
  int currentOutY;
  uint32_t nextOutY_srcStart;  // 16.16 fixed-point boundary for the next output row
  std::unique_ptr<uint32_t[]> rowAccum;
  std::unique_ptr<uint32_t[]> rowCount;

  int smoothNextOutY;
  int smoothPrevY;
  std::unique_ptr<uint8_t[]> smoothRows;
  uint8_t* smoothPrevRow;
  uint8_t* smoothCurrRow;
  uint8_t* smoothOutRow;

  std::unique_ptr<uint8_t[]> bmpRow;

  std::unique_ptr<AtkinsonDitherer> atkinsonDitherer;
  std::unique_ptr<FloydSteinbergDitherer> fsDitherer;
  std::unique_ptr<Atkinson1BitDitherer> atkinson1BitDitherer;

  uint8_t rowsSinceYield;
  uint8_t blocksSinceYield;
  bool error;
};

static void yieldDuringDecode(BmpConvertCtx* ctx) {
  if (++ctx->rowsSinceYield < 8) return;
  ctx->rowsSinceYield = 0;
  yieldToIdle();
}

static void yieldDuringDecodeBlock(BmpConvertCtx* ctx) {
  if (++ctx->blocksSinceYield < 16) return;
  ctx->blocksSinceYield = 0;
  yieldToIdle();
}

// Write a fully-assembled output row (grayscale bytes, length outWidth) to BMP
static void writeOutputRow(BmpConvertCtx* ctx, const uint8_t* srcRow, int outY) {
  memset(ctx->bmpRow.get(), 0, ctx->bytesPerRow);

  if (USE_8BIT_OUTPUT && !ctx->oneBit) {
    for (int x = 0; x < ctx->outWidth; x++) {
      ctx->bmpRow[x] = adjustPixel(srcRow[x]);
    }
  } else if (ctx->oneBit) {
    for (int x = 0; x < ctx->outWidth; x++) {
      const uint8_t bit = ctx->atkinson1BitDitherer ? ctx->atkinson1BitDitherer->processPixel(srcRow[x], x)
                                                    : quantize1bit(srcRow[x], x, outY);
      ctx->bmpRow[x / 8] |= (bit << (7 - (x % 8)));
    }
    if (ctx->atkinson1BitDitherer) ctx->atkinson1BitDitherer->nextRow();
  } else {
    for (int x = 0; x < ctx->outWidth; x++) {
      const uint8_t gray = adjustPixel(srcRow[x]);
      uint8_t twoBit;
      if (ctx->atkinsonDitherer) {
        twoBit = ctx->atkinsonDitherer->processPixel(gray, x);
      } else if (ctx->fsDitherer) {
        twoBit = ctx->fsDitherer->processPixel(gray, x);
      } else {
        twoBit = quantize(gray, x, outY);
      }
      ctx->bmpRow[(x * 2) / 8] |= (twoBit << (6 - ((x * 2) % 8)));
    }
    if (ctx->atkinsonDitherer)
      ctx->atkinsonDitherer->nextRow();
    else if (ctx->fsDitherer)
      ctx->fsDitherer->nextRow();
  }

  ctx->bmpOut->write(ctx->bmpRow.get(), ctx->bytesPerRow);
  yieldDuringDecode(ctx);
}

// Matches the progressive-JPEG smoothing used by JpegToFramebufferConverter, but stays
// local because cover generation streams dithered BMP rows instead of framebuffer pixels.
static uint32_t interpolationStep(const int srcSize, const int outSize) {
  if (srcSize <= 1 || outSize <= 1) return 0;
  return (static_cast<uint32_t>(srcSize - 1) << 16) / static_cast<uint32_t>(outSize - 1);
}

static uint32_t interpolatedSourceFp(const int outIndex, const int outSize, const int srcSize, const uint32_t step) {
  if (srcSize <= 1 || outSize <= 1) return 0;
  if (outIndex >= outSize - 1) return static_cast<uint32_t>(srcSize - 1) << 16;
  return static_cast<uint32_t>(outIndex) * step;
}

static void scaleRowLinear(BmpConvertCtx* ctx, const uint8_t* srcRow, uint8_t* dstRow) {
  for (int outX = 0; outX < ctx->outWidth; outX++) {
    const uint32_t srcX_fp = interpolatedSourceFp(outX, ctx->outWidth, ctx->srcWidth, ctx->smoothScaleX_fp);
    const int x0 = srcX_fp >> 16;
    const int x1 = (x0 + 1 < ctx->srcWidth) ? (x0 + 1) : x0;
    const uint32_t fx = srcX_fp & (FP_ONE - 1);
    dstRow[outX] = static_cast<uint8_t>((srcRow[x0] * (FP_ONE - fx) + srcRow[x1] * fx) >> 16);
  }
}

static void writeBlendedRow(BmpConvertCtx* ctx, const uint8_t* row0, const uint8_t* row1, const uint32_t fy,
                            const int outY) {
  const uint32_t invFy = FP_ONE - fy;
  for (int outX = 0; outX < ctx->outWidth; outX++) {
    ctx->smoothOutRow[outX] = static_cast<uint8_t>((row0[outX] * invFy + row1[outX] * fy) >> 16);
  }
  writeOutputRow(ctx, ctx->smoothOutRow, outY);
}

static void processSmoothSourceRow(BmpConvertCtx* ctx, const uint8_t* srcRow, const int srcY) {
  scaleRowLinear(ctx, srcRow, ctx->smoothCurrRow);

  if (ctx->smoothPrevY < 0) {
    uint8_t* tmp = ctx->smoothPrevRow;
    ctx->smoothPrevRow = ctx->smoothCurrRow;
    ctx->smoothCurrRow = tmp;
    ctx->smoothPrevY = srcY;
    if (ctx->srcHeight <= 1) {
      while (ctx->smoothNextOutY < ctx->outHeight) {
        writeOutputRow(ctx, ctx->smoothPrevRow, ctx->smoothNextOutY);
        ctx->smoothNextOutY++;
      }
      return;
    }
    return;
  }

  while (ctx->smoothNextOutY < ctx->outHeight) {
    const uint32_t srcY_fp =
        interpolatedSourceFp(ctx->smoothNextOutY, ctx->outHeight, ctx->srcHeight, ctx->smoothScaleY_fp);
    const int y0 = srcY_fp >> 16;
    const int y1 = (y0 + 1 < ctx->srcHeight) ? (y0 + 1) : y0;
    if (y1 > srcY) break;

    const uint8_t* row0 = (y0 == srcY) ? ctx->smoothCurrRow : ctx->smoothPrevRow;
    const uint8_t* row1 = (y1 == srcY) ? ctx->smoothCurrRow : ctx->smoothPrevRow;
    writeBlendedRow(ctx, row0, row1, srcY_fp & (FP_ONE - 1), ctx->smoothNextOutY);
    ctx->smoothNextOutY++;
  }

  uint8_t* tmp = ctx->smoothPrevRow;
  ctx->smoothPrevRow = ctx->smoothCurrRow;
  ctx->smoothCurrRow = tmp;
  ctx->smoothPrevY = srcY;
}

static void finishSmoothUpscale(BmpConvertCtx* ctx) {
  if (ctx->smoothPrevY < 0) {
    LOG_ERR("JPG", "No progressive rows decoded for smoothing");
    ctx->error = true;
    return;
  }

  while (ctx->smoothNextOutY < ctx->outHeight) {
    writeOutputRow(ctx, ctx->smoothPrevRow, ctx->smoothNextOutY);
    ctx->smoothNextOutY++;
  }
}

// Flush one scaled output row from Y-axis accumulators and advance currentOutY
static void flushScaledRow(BmpConvertCtx* ctx) {
  memset(ctx->bmpRow.get(), 0, ctx->bytesPerRow);

  if (USE_8BIT_OUTPUT && !ctx->oneBit) {
    for (int x = 0; x < ctx->outWidth; x++) {
      const uint8_t gray = (ctx->rowCount[x] > 0) ? (ctx->rowAccum[x] / ctx->rowCount[x]) : 0;
      ctx->bmpRow[x] = adjustPixel(gray);
    }
  } else if (ctx->oneBit) {
    for (int x = 0; x < ctx->outWidth; x++) {
      const uint8_t gray = (ctx->rowCount[x] > 0) ? (ctx->rowAccum[x] / ctx->rowCount[x]) : 0;
      const uint8_t bit = ctx->atkinson1BitDitherer ? ctx->atkinson1BitDitherer->processPixel(gray, x)
                                                    : quantize1bit(gray, x, ctx->currentOutY);
      ctx->bmpRow[x / 8] |= (bit << (7 - (x % 8)));
    }
    if (ctx->atkinson1BitDitherer) ctx->atkinson1BitDitherer->nextRow();
  } else {
    for (int x = 0; x < ctx->outWidth; x++) {
      const uint8_t gray = adjustPixel((ctx->rowCount[x] > 0) ? (ctx->rowAccum[x] / ctx->rowCount[x]) : 0);
      uint8_t twoBit;
      if (ctx->atkinsonDitherer) {
        twoBit = ctx->atkinsonDitherer->processPixel(gray, x);
      } else if (ctx->fsDitherer) {
        twoBit = ctx->fsDitherer->processPixel(gray, x);
      } else {
        twoBit = quantize(gray, x, ctx->currentOutY);
      }
      ctx->bmpRow[(x * 2) / 8] |= (twoBit << (6 - ((x * 2) % 8)));
    }
    if (ctx->atkinsonDitherer)
      ctx->atkinsonDitherer->nextRow();
    else if (ctx->fsDitherer)
      ctx->fsDitherer->nextRow();
  }

  ctx->bmpOut->write(ctx->bmpRow.get(), ctx->bytesPerRow);
  ctx->currentOutY++;
  yieldDuringDecode(ctx);
}

// JPEGDEC draw callback — receives one MCU-width × MCU-height block at a time,
// in left-to-right, top-to-bottom order (baseline JPEG).
// Accumulates columns into mcuBuf; once the last column arrives (completing the MCU
// row), applies scaling + dithering and writes packed BMP rows to bmpOut.
int bmpDrawCallback(BandBlock* pDraw) {
  auto* ctx = reinterpret_cast<BmpConvertCtx*>(pDraw->pUser);
  if (!ctx || ctx->error) return 0;
  yieldDuringDecodeBlock(ctx);

  const uint8_t* pixels = reinterpret_cast<uint8_t*>(pDraw->pPixels);
  const int stride = pDraw->iWidth;
  const int validW = pDraw->iWidthUsed;
  const int blockH = pDraw->iHeight;
  const int blockX = pDraw->x;
  const int blockY = pDraw->y;

  // Guard against unexpected callback geometry so we never index past row buffers.
  if (blockX < 0 || blockY < 0 || blockX >= ctx->srcWidth || blockY >= ctx->srcHeight) {
    LOG_ERR("JPG", "Unexpected JPEG block origin (%d,%d) for decode grid %dx%d", blockX, blockY, ctx->srcWidth,
            ctx->srcHeight);
    ctx->error = true;
    return 0;
  }

  // Copy block pixels into MCU row buffer
  for (int r = 0; r < blockH && r < MAX_MCU_HEIGHT; r++) {
    const int copyW = (blockX + validW <= ctx->srcWidth) ? validW : (ctx->srcWidth - blockX);
    if (copyW <= 0) continue;
    memcpy(ctx->mcuBuf.get() + r * ctx->srcWidth + blockX, pixels + r * stride, copyW);
  }

  // Wait for the last MCU column before processing any rows
  if (blockX + validW < ctx->srcWidth) return 1;

  // Process each complete source row in this MCU row
  const int endRow = blockY + blockH;

  for (int y = blockY; y < endRow && y < ctx->srcHeight; y++) {
    const uint8_t* srcRow = ctx->mcuBuf.get() + (y - blockY) * ctx->srcWidth;

    if (ctx->smoothUpscale) {
      processSmoothSourceRow(ctx, srcRow, y);
    } else if (!ctx->needsScaling) {
      // 1:1 — outWidth == srcWidth, write directly
      writeOutputRow(ctx, srcRow, y);
    } else {
      // Fixed-point area averaging on X axis
      for (int outX = 0; outX < ctx->outWidth; outX++) {
        const int srcXStart = (static_cast<uint32_t>(outX) * ctx->scaleX_fp) >> 16;
        const int srcXEnd = (static_cast<uint32_t>(outX + 1) * ctx->scaleX_fp) >> 16;
        int sum = 0;
        int count = 0;
        for (int srcX = srcXStart; srcX < srcXEnd && srcX < ctx->srcWidth; srcX++) {
          sum += srcRow[srcX];
          count++;
        }
        if (count == 0 && srcXStart < ctx->srcWidth) {
          sum = srcRow[srcXStart];
          count = 1;
        }
        ctx->rowAccum[outX] += sum;
        ctx->rowCount[outX] += count;
      }

      // Flush output row(s) whose Y boundary we've crossed
      const uint32_t srcY_fp = static_cast<uint32_t>(y + 1) << 16;
      while (srcY_fp >= ctx->nextOutY_srcStart && ctx->currentOutY < ctx->outHeight) {
        flushScaledRow(ctx);
        ctx->nextOutY_srcStart = static_cast<uint32_t>(ctx->currentOutY + 1) * ctx->scaleY_fp;
        if (srcY_fp >= ctx->nextOutY_srcStart) continue;
        memset(ctx->rowAccum.get(), 0, ctx->outWidth * sizeof(uint32_t));
        memset(ctx->rowCount.get(), 0, ctx->outWidth * sizeof(uint32_t));
      }
    }
  }

  return ctx->error ? 0 : 1;
}

}  // namespace

// Internal implementation with configurable target size and bit depth
bool JpegToBmpConverter::jpegFileToBmpStreamInternal(HalFile& jpegFile, Print& bmpOut, int targetWidth,
                                                     int targetHeight, bool oneBit, bool crop) {
  LOG_DBG("JPG", "Converting JPEG to %s BMP (target: %dx%d)", oneBit ? "1-bit" : "2-bit", targetWidth, targetHeight);

  // No heap gate: that existed to fit an MCU-era decoder into an ESP32 heap. The whole
  // file is read into memory instead of being streamed through MCU callbacks.
  std::vector<uint8_t> file;
  {
    uint8_t chunk[16 * 1024];
    for (;;) {
      const int n = jpegFile.read(chunk, sizeof(chunk));
      if (n <= 0) break;
      file.insert(file.end(), chunk, chunk + n);
    }
  }
  if (file.size() < 4) {
    LOG_ERR("JPG", "Could not read JPEG (%u bytes)", (unsigned)file.size());
    return false;
  }

  BmpJpegError err;
  jpeg_decompress_struct cinfo;
  std::memset(&cinfo, 0, sizeof(cinfo));
  bool created = false;
  cinfo.err = jpeg_std_error(&err.pub);
  err.pub.error_exit = bmpJpegErrorExit;
  if (setjmp(err.escape)) {
    if (created) jpeg_destroy_decompress(&cinfo);
    LOG_ERR("JPG", "Cover decode failed: %s", err.message);
    return false;
  }

  jpeg_create_decompress(&cinfo);
  created = true;
  jpeg_mem_src(&cinfo, file.data(), static_cast<unsigned long>(file.size()));
  if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
    jpeg_destroy_decompress(&cinfo);
    LOG_ERR("JPG", "Not a JPEG");
    return false;
  }

  const int srcWidth = static_cast<int>(cinfo.image_width);
  const int srcHeight = static_cast<int>(cinfo.image_height);
  // Progressive is decoded at FULL resolution now: the old 1/8 path was a decoder
  // limitation (an 8x blur on every progressive cover), not a requirement.
  const int decodedSrcWidth = srcWidth;
  const int decodedSrcHeight = srcHeight;
  int rc = 1;

  LOG_DBG("JPG", "JPEG dimensions: %dx%d", srcWidth, srcHeight);

  constexpr int MAX_IMAGE_WIDTH = 8192;  // was 2048 (device refusal); bound only
  constexpr int MAX_IMAGE_HEIGHT = 8192;  // was 3072 (device refusal); bound only

  if (srcWidth <= 0 || srcHeight <= 0 || srcWidth > MAX_IMAGE_WIDTH || srcHeight > MAX_IMAGE_HEIGHT) {
    LOG_DBG("JPG", "Image too large or invalid (%dx%d), max supported: %dx%d", srcWidth, srcHeight, MAX_IMAGE_WIDTH,
            MAX_IMAGE_HEIGHT);
    return false;
  }

  // Calculate output dimensions (pre-scale to fit display exactly)
  int outWidth = srcWidth;
  int outHeight = srcHeight;
  if (targetWidth <= 0 || targetHeight <= 0) {
    // Without an explicit target, keep decoder-native dimensions.
    outWidth = decodedSrcWidth;
    outHeight = decodedSrcHeight;
  }

  const int scaleSrcWidth = decodedSrcWidth;
  const int scaleSrcHeight = decodedSrcHeight;

  uint32_t scaleX_fp = 65536;  // 1.0 in 16.16 fixed point
  uint32_t scaleY_fp = 65536;
  bool needsScaling = false;

  if (targetWidth > 0 && targetHeight > 0 && (srcWidth != targetWidth || srcHeight != targetHeight)) {
    const float scaleToFitWidth = static_cast<float>(targetWidth) / srcWidth;
    const float scaleToFitHeight = static_cast<float>(targetHeight) / srcHeight;
    float scale = 1.0f;
    if (crop) {
      scale = (scaleToFitWidth > scaleToFitHeight) ? scaleToFitWidth : scaleToFitHeight;
    } else {
      scale = (scaleToFitWidth < scaleToFitHeight) ? scaleToFitWidth : scaleToFitHeight;
    }

    outWidth = static_cast<int>(srcWidth * scale);
    outHeight = static_cast<int>(srcHeight * scale);
    if (outWidth < 1) outWidth = 1;
    if (outHeight < 1) outHeight = 1;

    LOG_DBG("JPG", "Scaling source %dx%d (decode grid %dx%d) -> %dx%d (target %dx%d)", srcWidth, srcHeight,
            scaleSrcWidth, scaleSrcHeight, outWidth, outHeight, targetWidth, targetHeight);
  }

  if (scaleSrcWidth != outWidth || scaleSrcHeight != outHeight) {
    scaleX_fp = (static_cast<uint32_t>(scaleSrcWidth) << 16) / outWidth;
    scaleY_fp = (static_cast<uint32_t>(scaleSrcHeight) << 16) / outHeight;
    needsScaling = true;
  }

  // Used to be gated on progressiveDecode, because progressive arrived at 1/8 resolution
  // and every cover needed heavy smoothing back up. Progressive now decodes at full
  // resolution, so the gate is simply "are we upscaling?".
  const bool smoothUpscale = needsScaling && scaleSrcWidth <= outWidth && scaleSrcHeight <= outHeight;

  // Write BMP header with output dimensions
  int bytesPerRow;
  if (USE_8BIT_OUTPUT && !oneBit) {
    writeBmpHeader8bit(bmpOut, outWidth, outHeight);
    bytesPerRow = (outWidth + 3) / 4 * 4;
  } else if (oneBit) {
    writeBmpHeader1bit(bmpOut, outWidth, outHeight);
    bytesPerRow = (outWidth + 31) / 32 * 4;
  } else {
    writeBmpHeader2bit(bmpOut, outWidth, outHeight);
    bytesPerRow = (outWidth * 2 + 31) / 32 * 4;
  }

  BmpConvertCtx ctx = {};
  ctx.bmpOut = &bmpOut;
  ctx.srcWidth = scaleSrcWidth;
  ctx.srcHeight = scaleSrcHeight;
  ctx.outWidth = outWidth;
  ctx.outHeight = outHeight;
  ctx.oneBit = oneBit;
  ctx.bytesPerRow = bytesPerRow;
  ctx.needsScaling = needsScaling;
  ctx.scaleX_fp = scaleX_fp;
  ctx.scaleY_fp = scaleY_fp;
  ctx.smoothUpscale = smoothUpscale;
  ctx.smoothScaleX_fp = interpolationStep(ctx.srcWidth, outWidth);
  ctx.smoothScaleY_fp = interpolationStep(ctx.srcHeight, outHeight);
  ctx.smoothNextOutY = 0;
  ctx.smoothPrevY = -1;
  ctx.rowsSinceYield = 0;
  ctx.blocksSinceYield = 0;
  ctx.error = false;

  // MCU row buffer: MAX_MCU_HEIGHT rows × decoded srcWidth columns of grayscale
  ctx.mcuBuf = makeUniqueNoThrow<uint8_t[]>(MAX_MCU_HEIGHT * ctx.srcWidth);
  if (!ctx.mcuBuf) {
    LOG_ERR("JPG", "OOM: MCU buffer (%d bytes)", MAX_MCU_HEIGHT * ctx.srcWidth);
    return false;
  }
  memset(ctx.mcuBuf.get(), 0, MAX_MCU_HEIGHT * ctx.srcWidth);

  ctx.bmpRow = makeUniqueNoThrow<uint8_t[]>(bytesPerRow);
  if (!ctx.bmpRow) {
    LOG_ERR("JPG", "OOM: BMP row buffer");
    return false;
  }

  if (smoothUpscale) {
    // One contiguous allocation avoids three heap blocks while keeping smoothing line-buffered.
    const size_t smoothRowsBytes = static_cast<size_t>(outWidth) * 3;
    ctx.smoothRows = makeUniqueNoThrow<uint8_t[]>(smoothRowsBytes);
    if (!ctx.smoothRows) {
      LOG_ERR("JPG", "OOM: progressive smoothing buffers");
      return false;
    }
    ctx.smoothPrevRow = ctx.smoothRows.get();
    ctx.smoothCurrRow = ctx.smoothPrevRow + outWidth;
    ctx.smoothOutRow = ctx.smoothCurrRow + outWidth;
    LOG_DBG("JPG", "Progressive smoothing: %dx%d -> %dx%d, buffers=%u bytes", ctx.srcWidth, ctx.srcHeight, outWidth,
            outHeight, static_cast<unsigned>(smoothRowsBytes));
  } else if (needsScaling) {
    ctx.rowAccum = makeUniqueNoThrow<uint32_t[]>(outWidth);
    ctx.rowCount = makeUniqueNoThrow<uint32_t[]>(outWidth);
    if (!ctx.rowAccum || !ctx.rowCount) {
      LOG_ERR("JPG", "OOM: scaling buffers");
      return false;
    }
    ctx.nextOutY_srcStart = scaleY_fp;
  }

  if (oneBit) {
    ctx.atkinson1BitDitherer = makeUniqueNoThrow<Atkinson1BitDitherer>(outWidth);
    if (!ctx.atkinson1BitDitherer) {
      LOG_ERR("JPG", "OOM: Atkinson1BitDitherer");
      return false;
    }
  } else if (!USE_8BIT_OUTPUT) {
    if (USE_ATKINSON) {
      ctx.atkinsonDitherer = makeUniqueNoThrow<AtkinsonDitherer>(outWidth);
      if (!ctx.atkinsonDitherer) {
        LOG_ERR("JPG", "OOM: AtkinsonDitherer");
        return false;
      }
    } else if (USE_FLOYD_STEINBERG) {
      ctx.fsDitherer = makeUniqueNoThrow<FloydSteinbergDitherer>(outWidth);
      if (!ctx.fsDitherer) {
        LOG_ERR("JPG", "OOM: FloydSteinbergDitherer");
        return false;
      }
    }
  }

  // Whole-frame greyscale decode with the integer ISLOW IDCT (SIMD is off in both
  // builds, so host and wasm produce identical pixels).
  cinfo.out_color_space = JCS_GRAYSCALE;
  cinfo.dct_method = JDCT_ISLOW;
  jpeg_start_decompress(&cinfo);

  const int decW = static_cast<int>(cinfo.output_width);
  const int decH = static_cast<int>(cinfo.output_height);
  if (decW != decodedSrcWidth || decH != decodedSrcHeight) {
    LOG_ERR("JPG", "Decoded %dx%d but expected %dx%d", decW, decH, decodedSrcWidth, decodedSrcHeight);
    jpeg_destroy_decompress(&cinfo);
    return false;
  }

  std::vector<uint8_t> frame(static_cast<size_t>(decW) * static_cast<size_t>(decH));
  while (cinfo.output_scanline < cinfo.output_height) {
    JSAMPROW rows[1] = {frame.data() + static_cast<size_t>(cinfo.output_scanline) * decW};
    jpeg_read_scanlines(&cinfo, rows, 1);
  }
  jpeg_finish_decompress(&cinfo);
  jpeg_destroy_decompress(&cinfo);
  created = false;

  // Feed the unchanged MCU-row callback one band at a time. blockX = 0 makes the
  // callback's "last MCU column" test true immediately, so each call processes exactly
  // its own rows; bandH is capped by MAX_MCU_HEIGHT, the height of its row buffer.
  for (int bandY = 0; bandY < decH && !ctx.error; bandY += MAX_MCU_HEIGHT) {
    const int bandH = std::min(MAX_MCU_HEIGHT, decH - bandY);
    BandBlock draw{};
    draw.pUser = &ctx;
    draw.x = 0;
    draw.y = bandY;
    draw.iWidth = decW;
    draw.iWidthUsed = decW;
    draw.iHeight = bandH;
    draw.pPixels = reinterpret_cast<uint16_t*>(frame.data() + static_cast<size_t>(bandY) * decW);
    bmpDrawCallback(&draw);
  }
  rc = ctx.error ? 0 : 1;

  if (rc == 1 && ctx.smoothUpscale && !ctx.error) {
    finishSmoothUpscale(&ctx);
  }

  if (rc != 1 || ctx.error) {
    LOG_ERR("JPG", "JPEG decode failed (rc=%d)", rc);
    return false;
  }

  LOG_DBG("JPG", "Successfully converted JPEG to BMP");
  return true;
}

// Core function: Convert JPEG file to 2-bit BMP (uses default target size)
bool JpegToBmpConverter::jpegFileToBmpStream(HalFile& jpegFile, Print& bmpOut, bool crop) {
  // Use runtime display dimensions (swapped for portrait cover sizing)
  const int targetWidth = display.getDisplayHeight();
  const int targetHeight = display.getDisplayWidth();
  return jpegFileToBmpStreamInternal(jpegFile, bmpOut, targetWidth, targetHeight, false, crop);
}

// Convert with custom target size (for thumbnails, 2-bit)
bool JpegToBmpConverter::jpegFileToBmpStreamWithSize(HalFile& jpegFile, Print& bmpOut, int targetMaxWidth,
                                                     int targetMaxHeight) {
  return jpegFileToBmpStreamInternal(jpegFile, bmpOut, targetMaxWidth, targetMaxHeight, false);
}

// Convert to 1-bit BMP (black and white only, no grays) for fast home screen rendering
bool JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(HalFile& jpegFile, Print& bmpOut, int targetMaxWidth,
                                                         int targetMaxHeight) {
  return jpegFileToBmpStreamInternal(jpegFile, bmpOut, targetMaxWidth, targetMaxHeight, true, true);
}
