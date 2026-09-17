#include "PngToFramebufferConverter.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <PNGdec.h>  // remaining: the scanline decode path

#include <cstdlib>
#include <cstring>
#include <vector>

#include <png.h>
#include <memory>
#include <new>

#include "DirectPixelWriter.h"
#include "DitherUtils.h"

namespace {

// Context struct passed through PNGdec callbacks to avoid global mutable state.
// The draw callback receives this via pDraw->pUser (set by png.decode()).
// The file I/O callbacks receive the HalFile* via pFile->fHandle (set by pngOpen()).
struct PngContext {
  GfxRenderer* renderer{nullptr};
  const RenderConfig* config{nullptr};
  int screenWidth{0};
  int screenHeight{0};

  // Scaling state
  float scale{1.f};
  int srcWidth{0};
  int srcHeight{0};
  int dstWidth{0};
  int dstHeight{0};
  int lastDstY{-1};  // Track last rendered destination Y to avoid duplicates

  bool caching{false};

  uint8_t* grayLineBuffer{nullptr};
};

// File I/O callbacks use pFile->fHandle to access the HalFile*,
// avoiding the need for global file state.
void* pngOpenWithHandle(const char* filename, int32_t* size) {
  HalFile* f = new HalFile();
  if (!Storage.openFileForRead("PNG", std::string(filename), *f)) {
    delete f;
    return nullptr;
  }
  *size = f->size();
  return f;
}

void pngCloseWithHandle(void* handle) {
  HalFile* f = reinterpret_cast<HalFile*>(handle);
  if (f) {
    f->close();
    delete f;
  }
}

int32_t pngReadWithHandle(PNGFILE* pFile, uint8_t* pBuf, int32_t len) {
  HalFile* f = reinterpret_cast<HalFile*>(pFile->fHandle);
  if (!f) return 0;
  return f->read(pBuf, len);
}

int32_t pngSeekWithHandle(PNGFILE* pFile, int32_t pos) {
  HalFile* f = reinterpret_cast<HalFile*>(pFile->fHandle);
  if (!f) return -1;
  return f->seek(pos);
}

// The PNG decoder (PNGdec) is ~42 KB due to internal zlib decompression buffers.
// We heap-allocate it on demand rather than using a static instance, so this memory
// is only consumed while actually decoding/querying PNG images. This is critical on
// the ESP32-C3 where total RAM is ~320 KB.
constexpr size_t PNG_DECODER_APPROX_SIZE = 44 * 1024;                          // ~42 KB + overhead
constexpr size_t MIN_FREE_HEAP_FOR_PNG = PNG_DECODER_APPROX_SIZE + 16 * 1024;  // decoder + 16 KB headroom

// PNGdec keeps TWO scanlines in its internal ucPixels buffer (current + previous)
// and each scanline includes a leading filter byte.
// Required storage is therefore approximately: 2 * (pitch + 1) + alignment slack.
// If PNG_MAX_BUFFERED_PIXELS is smaller than this requirement for a given image,
// PNGdec can overrun its internal buffer before our draw callback executes.
int bytesPerPixelFromType(int pixelType) {
  switch (pixelType) {
    case PNG_PIXEL_TRUECOLOR:
      return 3;
    case PNG_PIXEL_GRAY_ALPHA:
      return 2;
    case PNG_PIXEL_TRUECOLOR_ALPHA:
      return 4;
    case PNG_PIXEL_GRAYSCALE:
    case PNG_PIXEL_INDEXED:
    default:
      return 1;
  }
}

int packedRowBytes(int srcWidth, int bitsPerSample) { return (srcWidth * bitsPerSample + 7) / 8; }

int requiredPngInternalBufferBytes(int srcWidth, int pixelType, int bitsPerSample) {
  // +1 filter byte per scanline, *2 for current+previous lines, +32 for alignment margin.
  int pitch = srcWidth * bytesPerPixelFromType(pixelType);
  if ((pixelType == PNG_PIXEL_GRAYSCALE || pixelType == PNG_PIXEL_INDEXED) && bitsPerSample < 8) {
    pitch = packedRowBytes(srcWidth, bitsPerSample);
  }
  return ((pitch + 1) * 2) + 32;
}

bool isSupportedBitDepth(int pixelType, int bitsPerSample) {
  if (bitsPerSample == 8) return true;
  if (bitsPerSample != 1 && bitsPerSample != 2 && bitsPerSample != 4) return false;
  return pixelType == PNG_PIXEL_GRAYSCALE || pixelType == PNG_PIXEL_INDEXED;
}

uint8_t readPackedSample(const uint8_t* pixels, int x, int bitsPerSample) {
  if (bitsPerSample == 8) return pixels[x];

  const int bitOffset = x * bitsPerSample;
  const int shift = 8 - bitsPerSample - (bitOffset & 7);
  const uint8_t mask = (1U << bitsPerSample) - 1;
  return (pixels[bitOffset >> 3] >> shift) & mask;
}

uint8_t expandSampleToByte(uint8_t sample, int bitsPerSample) {
  if (bitsPerSample == 8) return sample;
  const uint8_t maxSample = (1U << bitsPerSample) - 1;
  return static_cast<uint8_t>((sample * 255U) / maxSample);
}

// Convert entire source line to grayscale with alpha blending to white background.
// Low-bit-depth grayscale/indexed scanlines are packed most-significant sample first.
// For indexed PNGs with tRNS chunk, alpha values are stored at palette[768] onwards.
// Processing the whole line at once improves cache locality and reduces per-pixel overhead.
void convertLineToGray(const uint8_t* pPixels, uint8_t* grayLine, int width, int pixelType, int bitsPerSample,
                       uint8_t* palette, int hasAlpha) {
  switch (pixelType) {
    case PNG_PIXEL_GRAYSCALE:
      if (bitsPerSample == 8) {
        memcpy(grayLine, pPixels, width);
      } else {
        for (int x = 0; x < width; x++) {
          grayLine[x] = expandSampleToByte(readPackedSample(pPixels, x, bitsPerSample), bitsPerSample);
        }
      }
      break;

    case PNG_PIXEL_TRUECOLOR:
      for (int x = 0; x < width; x++) {
        const uint8_t* p = &pPixels[x * 3];
        grayLine[x] = (uint8_t)((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
      }
      break;

    case PNG_PIXEL_INDEXED:
      if (palette) {
        if (hasAlpha) {
          for (int x = 0; x < width; x++) {
            uint8_t idx = readPackedSample(pPixels, x, bitsPerSample);
            uint8_t* p = &palette[idx * 3];
            uint8_t gray = (uint8_t)((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
            uint8_t alpha = palette[768 + idx];
            grayLine[x] = (uint8_t)((gray * alpha + 255 * (255 - alpha)) / 255);
          }
        } else {
          for (int x = 0; x < width; x++) {
            uint8_t idx = readPackedSample(pPixels, x, bitsPerSample);
            uint8_t* p = &palette[idx * 3];
            grayLine[x] = (uint8_t)((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
          }
        }
      } else {
        for (int x = 0; x < width; x++) {
          grayLine[x] = expandSampleToByte(readPackedSample(pPixels, x, bitsPerSample), bitsPerSample);
        }
      }
      break;

    case PNG_PIXEL_GRAY_ALPHA:
      for (int x = 0; x < width; x++) {
        uint8_t gray = pPixels[x * 2];
        uint8_t alpha = pPixels[x * 2 + 1];
        grayLine[x] = (uint8_t)((gray * alpha + 255 * (255 - alpha)) / 255);
      }
      break;

    case PNG_PIXEL_TRUECOLOR_ALPHA:
      for (int x = 0; x < width; x++) {
        const uint8_t* p = &pPixels[x * 4];
        uint8_t gray = (uint8_t)((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
        uint8_t alpha = p[3];
        grayLine[x] = (uint8_t)((gray * alpha + 255 * (255 - alpha)) / 255);
      }
      break;

    default:
      memset(grayLine, 128, width);
      break;
  }
}

int pngDrawCallback(PNGDRAW* pDraw) {
  PngContext* ctx = reinterpret_cast<PngContext*>(pDraw->pUser);
  if (!ctx || !ctx->config || !ctx->renderer || !ctx->grayLineBuffer) return 0;

  int srcY = pDraw->y;
  int srcWidth = ctx->srcWidth;

  // Map source rows with the exact output-height ratio. During downscaling,
  // multiple source rows can select the same output row; during upscaling, one
  // source row must be repeated across every output row in its range. Emitting
  // only the first row of an upscale leaves zero-filled (black) gaps in the
  // streamed pixel cache.
  int firstDstY = (srcY * ctx->dstHeight) / ctx->srcHeight;
  int endDstY = firstDstY + 1;
  if (ctx->dstHeight > ctx->srcHeight) {
    endDstY = ((srcY + 1) * ctx->dstHeight) / ctx->srcHeight;
  }

  if (firstDstY <= ctx->lastDstY) firstDstY = ctx->lastDstY + 1;
  if (firstDstY >= endDstY || firstDstY >= ctx->dstHeight) return 1;
  if (endDstY > ctx->dstHeight) endDstY = ctx->dstHeight;

  // Convert entire source line to grayscale (improves cache locality)
  convertLineToGray(pDraw->pPixels, ctx->grayLineBuffer, srcWidth, pDraw->iPixelType, pDraw->iBpp, pDraw->pPalette,
                    pDraw->iHasAlpha);

  // Render scaled rows using Bresenham-style integer stepping (no floating-point division)
  int dstWidth = ctx->dstWidth;
  int outXBase = ctx->config->x;
  int screenWidth = ctx->screenWidth;
  bool useDithering = ctx->config->useDithering;

  // Pre-compute orientation and render-mode state once per callback.
  DirectPixelWriter pw;
  pw.init(*ctx->renderer);

  for (int dstY = firstDstY; dstY < endDstY; dstY++) {
    ctx->lastDstY = dstY;
    int outY = ctx->config->y + dstY;
    if (outY >= ctx->screenHeight) continue;

    pw.beginRow(outY);

    // The cache streams to disk one row at a time. Flushing rows below this one
    // (PNGdec delivers scanlines top to bottom) repositions the single-row band.
    // A flush failure stops caching for the rest of the decode so we never write
    // past the band buffer; finalize() then drops the partial file.

    int srcX = 0;
    int error = 0;

    for (int dstX = 0; dstX < dstWidth; dstX++) {
      int outX = outXBase + dstX;
      if (outX < screenWidth) {
        uint8_t gray = ctx->grayLineBuffer[srcX];

        uint8_t ditheredGray;
        if (useDithering) {
          ditheredGray = applyOrderedDither4Level(gray, outX, outY);
        } else {
          ditheredGray = quantizeToLevel(gray);
        }
        pw.writePixel(outX, ditheredGray);
      }

      // Bresenham-style stepping: advance srcX based on ratio srcWidth/dstWidth
      error += srcWidth;
      while (error >= dstWidth) {
        error -= dstWidth;
        srcX++;
      }
    }
  }

  return 1;
}

}  // namespace


// ---------------------------------------------------------------------------
// libpng plumbing.
//
// Unlike JPEG - where implementations legitimately differ in IDCT rounding, which is why
// libjpeg-turbo is vendored with SIMD off - PNG decoding is exact integer arithmetic and
// inflate is deterministic, so the toolchain's own libpng (brew on host, the emscripten
// port for wasm) produces bit-identical pixels.
// ---------------------------------------------------------------------------

struct PngMemReader {
  const std::vector<uint8_t>* data;
  size_t offset;
};

void pngMemReadFn(png_structp png, png_bytep out, png_size_t count) {
  PngMemReader* reader = static_cast<PngMemReader*>(png_get_io_ptr(png));
  if (!reader || !reader->data || reader->offset + count > reader->data->size()) {
    png_error(png, "PNG read past end of buffer");
    return;
  }
  std::memcpy(out, reader->data->data() + reader->offset, count);
  reader->offset += count;
}

// Whole-file read: no streaming, no decoder-object heap budget.
bool readWholeFilePng(const std::string& path, std::vector<uint8_t>& out) {
  HalFile f;
  if (!Storage.openFileForRead("PNG", path, f)) {
    LOG_ERR("PNG", "Failed to open %s", path.c_str());
    return false;
  }
  const int64_t size = f.size();
  if (size <= 0) {
    LOG_ERR("PNG", "Empty file: %s", path.c_str());
    f.close();
    return false;
  }
  out.resize(static_cast<size_t>(size));
  const int32_t got = f.read(out.data(), static_cast<int32_t>(out.size()));
  f.close();
  if (got != static_cast<int32_t>(size)) {
    LOG_ERR("PNG", "Short read on %s", path.c_str());
    return false;
  }
  return true;
}

bool PngToFramebufferConverter::getDimensionsStatic(const std::string& imagePath, ImageDimensions& out) {
  // Use getMaxAllocHeap() (largest contiguous block) instead of getFreeHeap()
  // (total free): the PNG decoder is a single ~44 KB allocation, so total free
  // can be misleading on a fragmented heap — the 4× repeated PNG-decoder alloc
  // failures we saw on ko.17 were exactly this case.
  const size_t maxAlloc = ESP.getMaxAllocHeap();
  if (maxAlloc < MIN_FREE_HEAP_FOR_PNG) {
    LOG_ERR("PNG", "Not enough contiguous heap for PNG decoder (maxAlloc %u, need %u)", maxAlloc,
            MIN_FREE_HEAP_FOR_PNG);
    return false;
  }

  // Header-only probe: libpng reads and parses the IHDR from a whole-file buffer. The
  // old path allocated a ~42 KB PNGdec (plus its zlib state) just to read two integers.
  std::vector<uint8_t> file;
  if (!readWholeFilePng(imagePath, file)) return false;

  png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  if (!png) {
    LOG_ERR("PNG", "Failed to create PNG read struct");
    return false;
  }
  png_infop info = png_create_info_struct(png);
  if (!info) {
    png_destroy_read_struct(&png, nullptr, nullptr);
    return false;
  }
  // Canonical libpng error handling: libpng's default error function longjmps here on a
  // malformed file, so a bad image fails this probe instead of the process.
  if (setjmp(png_jmpbuf(png))) {
    png_destroy_read_struct(&png, &info, nullptr);
    LOG_ERR("PNG", "Bad PNG header: %s", imagePath.c_str());
    return false;
  }

  PngMemReader reader{&file, 0};
  png_set_read_fn(png, &reader, pngMemReadFn);
  png_read_info(png, info);
  out.width = static_cast<int>(png_get_image_width(png, info));
  out.height = static_cast<int>(png_get_image_height(png, info));
  png_destroy_read_struct(&png, &info, nullptr);

  return true;
}

bool PngToFramebufferConverter::decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer,
                                                    const RenderConfig& config) {
  LOG_DBG("PNG", "Decoding PNG: %s", imagePath.c_str());

  int rc = 0;

  // Use getMaxAllocHeap() (largest contiguous block) instead of getFreeHeap()
  // (total free): the PNG decoder is a single ~44 KB allocation, so total free
  // can be misleading on a fragmented heap — the 4× repeated PNG-decoder alloc
  // failures we saw on ko.17 were exactly this case.
  const size_t maxAlloc = ESP.getMaxAllocHeap();
  if (maxAlloc < MIN_FREE_HEAP_FOR_PNG) {
    LOG_ERR("PNG", "Not enough contiguous heap for PNG decoder (maxAlloc %u, need %u)", maxAlloc,
            MIN_FREE_HEAP_FOR_PNG);
    return false;
  }

  // Heap-allocate PNG decoder (~42 KB) - freed at end of function

  PngContext ctx;
  ctx.renderer = &renderer;
  ctx.config = &config;
  ctx.screenWidth = renderer.getScreenWidth();
  ctx.screenHeight = renderer.getScreenHeight();

  if (rc != PNG_SUCCESS) {
    LOG_ERR("PNG", "Failed to open PNG: %d", rc);
    return false;
  }

    // Whole-file read, then a header-only libpng probe for the dimensions. Dimensions feed
  // pagination, so this must agree with PNGdec exactly - verified against the stage 1 gate.
  std::vector<uint8_t> file;
  if (!readWholeFilePng(imagePath, file)) return false;

  int pngW = 0;
  int pngH = 0;
  {
    png_structp probe = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    png_infop probeInfo = probe ? png_create_info_struct(probe) : nullptr;
    if (!probe || !probeInfo) {
      if (probe) png_destroy_read_struct(&probe, nullptr, nullptr);
      LOG_ERR("PNG", "Failed to create PNG read struct: %s", imagePath.c_str());
      return false;
    }
    if (setjmp(png_jmpbuf(probe))) {
      png_destroy_read_struct(&probe, &probeInfo, nullptr);
      LOG_ERR("PNG", "Bad PNG header: %s", imagePath.c_str());
      return false;
    }
    PngMemReader reader{&file, 0};
    png_set_read_fn(probe, &reader, pngMemReadFn);
    png_read_info(probe, probeInfo);
    pngW = static_cast<int>(png_get_image_width(probe, probeInfo));
    pngH = static_cast<int>(png_get_image_height(probe, probeInfo));
    png_destroy_read_struct(&probe, &probeInfo, nullptr);
  }

  if (!validateImageDimensions(pngW, pngH, "PNG")) {
    return false;
  }

  // Calculate output dimensions
  ctx.srcWidth = pngW;
  ctx.srcHeight = pngH;

  if (config.useExactDimensions && config.maxWidth > 0 && config.maxHeight > 0) {
    // Use exact dimensions as specified (avoids rounding mismatches with pre-calculated sizes)
    ctx.dstWidth = config.maxWidth;
    ctx.dstHeight = config.maxHeight;
    ctx.scale = (float)ctx.dstWidth / ctx.srcWidth;
  } else {
    // Calculate scale factor to fit within maxWidth/maxHeight
    float scaleX = (float)config.maxWidth / ctx.srcWidth;
    float scaleY = (float)config.maxHeight / ctx.srcHeight;
    ctx.scale = (scaleX < scaleY) ? scaleX : scaleY;
    if (ctx.scale > 1.0f) ctx.scale = 1.0f;  // Don't upscale

    ctx.dstWidth = (int)(ctx.srcWidth * ctx.scale);
    ctx.dstHeight = (int)(ctx.srcHeight * ctx.scale);
  }
  ctx.lastDstY = -1;  // Reset row tracking

  const int pixelType = PNG_PIXEL_GRAYSCALE;
  const int bitsPerSample = 8;
  LOG_DBG("PNG", "PNG %dx%d -> %dx%d (scale %.2f), type: %d, bpp: %d", ctx.srcWidth, ctx.srcHeight, ctx.dstWidth,
          ctx.dstHeight, ctx.scale, pixelType, bitsPerSample);

  const int requiredInternal = requiredPngInternalBufferBytes(ctx.srcWidth, pixelType, bitsPerSample);

  if (!isSupportedBitDepth(pixelType, bitsPerSample)) {
    warnUnsupportedFeature(
        "bit depth (" + std::to_string(bitsPerSample) + "bpp) for pixel type " + std::to_string(pixelType), imagePath);
    return false;
  }

  // The converter expands each source row to 8-bit grayscale before dithering,
  // so this scratch buffer is sized by source pixels even when PNGdec reads a
  // packed 1/2/4-bit row internally.
  // No device-era cap here. The old limit was PNGdec's fixed internal buffer
  // (PNG_MAX_BUFFERED_PIXELS), which rejected any image wider than ~1281 px outright.
  // Nothing in this port has a fixed row buffer, so the scratch row is simply sized by
  // the source width.
  const size_t grayBufSize = static_cast<size_t>(ctx.srcWidth);

  auto grayLineBuffer = makeUniqueNoThrow<uint8_t[]>(grayBufSize);
  if (!grayLineBuffer) {
    LOG_ERR("PNG", "Failed to allocate gray line buffer");
    return false;
  }
  ctx.grayLineBuffer = grayLineBuffer.get();

  // Stream the pixel cache to disk. PNGdec delivers source scanlines top to
  // bottom and we emit at most one (downscaled) output row per callback, so the
  // band only needs a single row. Streaming keeps the working set tiny, so
  // unlike the old full-image buffer it neither competes with the ~44KB decoder
  // nor forces larger images to skip caching - which previously meant a full
  // re-decode on every one of an image page's ~14 render passes.

  unsigned long decodeStart = millis();
    // libpng, one row at a time, into the UNCHANGED callback: it is already per-scanline
  // (pDraw->y) and does all of its own up/downscale row mapping, so only the producer of
  // the pixels changes. The synthetic block carries exactly the fields that callback
  // reads: y, pPixels, iBpp, iPixelType, iHasAlpha, pPalette, pUser.
  rc = 1;
  {
    png_structp pngRead = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    png_infop pngInfo = pngRead ? png_create_info_struct(pngRead) : nullptr;
    if (!pngRead || !pngInfo) {
      if (pngRead) png_destroy_read_struct(&pngRead, nullptr, nullptr);
      LOG_ERR("PNG", "Failed to create PNG read struct");
    } else if (setjmp(png_jmpbuf(pngRead))) {
      png_destroy_read_struct(&pngRead, &pngInfo, nullptr);
      LOG_ERR("PNG", "PNG decode failed: %s", imagePath.c_str());
    } else {
      PngMemReader rowReader{&file, 0};
      png_set_read_fn(pngRead, &rowReader, pngMemReadFn);
      png_read_info(pngRead, pngInfo);

      // Normalise to 8-bit greyscale. Alpha is KEPT as a channel, never stripped: the
      // callback composites it (it took iHasAlpha from PNGdec too), so stripping would
      // change compositing.
      const int bitDepth = png_get_bit_depth(pngRead, pngInfo);
      const int colorType = png_get_color_type(pngRead, pngInfo);
      const bool indexed = (colorType == PNG_COLOR_TYPE_PALETTE);
      if (bitDepth == 16) png_set_strip_16(pngRead);
      if (indexed) png_set_palette_to_rgb(pngRead);
      if (colorType == PNG_COLOR_TYPE_GRAY && bitDepth < 8) png_set_expand_gray_1_2_4_to_8(pngRead);
      if (colorType == PNG_COLOR_TYPE_RGB || colorType == PNG_COLOR_TYPE_RGB_ALPHA || indexed) {
        png_set_rgb_to_gray_fixed(pngRead, 1, -1, -1);  // default weights
      }
      if (png_get_valid(pngRead, pngInfo, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(pngRead);
      const int passes = png_set_interlace_handling(pngRead);
      png_read_update_info(pngRead, pngInfo);
      const bool hasAlpha = (png_get_color_type(pngRead, pngInfo) & PNG_COLOR_MASK_ALPHA) != 0;

      std::vector<uint8_t> rowBuf(static_cast<size_t>(pngW) * (hasAlpha ? 2u : 1u));
      for (int pass = 0; pass < passes; pass++) {
        for (int y = 0; y < pngH; y++) {
          png_read_row(pngRead, rowBuf.data(), nullptr);
          if (pass != passes - 1) continue;  // only the final pass leaves dst complete
          PNGDRAW draw{};
          draw.pUser = &ctx;
          draw.y = y;
          draw.pPixels = rowBuf.data();
          draw.iBpp = 8;
          draw.iHasAlpha = hasAlpha ? 1 : 0;
          draw.iPixelType = hasAlpha ? PNG_PIXEL_GRAY_ALPHA : PNG_PIXEL_GRAYSCALE;
          draw.pPalette = nullptr;
          pngDrawCallback(&draw);
        }
      }
      png_read_end(pngRead, nullptr);
      png_destroy_read_struct(&pngRead, &pngInfo, nullptr);
      rc = 0;
    }
  }
  unsigned long decodeTime = millis() - decodeStart;

  ctx.grayLineBuffer = nullptr;

  if (rc != PNG_SUCCESS) {
    LOG_ERR("PNG", "Decode failed: %d", rc);
    return false;
  }

  LOG_DBG("PNG", "PNG decoding complete - render time: %lu ms", decodeTime);


  return true;
}

bool PngToFramebufferConverter::supportsFormat(const std::string& extension) {
  return FsHelpers::hasPngExtension(extension);
}
