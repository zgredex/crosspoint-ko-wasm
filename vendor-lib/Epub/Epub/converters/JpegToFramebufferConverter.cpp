#include "JpegToFramebufferConverter.h"
#include "ImagePerf.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include "BandBlock.h"
#include <Logging.h>
#include <Memory.h>

#include <csetjmp>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <vector>

#include <jpeglib.h>

#include "DirectPixelWriter.h"
#include "DitherUtils.h"
#include "ImageDither.h"

namespace {

// Context struct passed through JPEGDEC callbacks to avoid global mutable state.
// The draw callback receives this via pDraw->pUser (set by setUserPointer()).
// The file I/O callbacks receive the HalFile* via pFile->fHandle (set by jpegOpen()).
struct JpegContext {
  GfxRenderer* renderer{nullptr};
  // Dither model + tone depth for this image (one instance per decode: the diffusion
  // models keep per-image error rows). Set in decodeToFramebuffer.
  std::unique_ptr<ko::ImageDitherer> dither;
  const RenderConfig* config{nullptr};
  int screenWidth{0};
  int screenHeight{0};

  // Source dimensions after JPEGDEC's built-in scaling
  int scaledSrcWidth{0};
  int scaledSrcHeight{0};

  // Final output dimensions
  int dstWidth{0};
  int dstHeight{0};

  // Fine scale in 16.16 fixed-point (ESP32-C3 has no FPU).
  // X and Y axes use separate scale factors: the aspect ratio of the output (dstWidth/dstHeight)
  // may differ from the source (srcWidth/srcHeight) due to integer rounding of displayHeight.
  // Using a single (X-based) scale for both axes causes the wrong srcRow to be skipped
  // during nearest-neighbor downscaling, potentially losing critical image content.
  int32_t fineScaleFPX{1 << 16};  // X: src -> dst column mapping
  int32_t invScaleFPX{1 << 16};   // X: dst -> src column mapping
  int32_t fineScaleFPY{1 << 16};  // Y: src -> dst row mapping
  int32_t invScaleFPY{1 << 16};   // Y: dst -> src row mapping

};

// ---------------------------------------------------------------------------
// libjpeg-turbo, decoded from a whole-file buffer via jpeg_mem_src.
//
// The old decoder (JPEGDEC) streamed MCU bands through file callbacks because it
// had fixed internal buffers, and that banding is why the sampling math below is
// block-relative. We keep that math untouched and simply hand it the whole frame
// as a single band via a synthetic BandBlock (x=0, y=0), so `x - blockX` and
// `y - blockY` become the identity and the destination box cannot drift.
// ---------------------------------------------------------------------------

// libjpeg calls error_exit() — which is exit() — on malformed data unless the error
// manager is replaced. A corrupt image must fail the decode, never the process
// (and in wasm, never the whole conversion).
struct JpegErrorHandler {
  jpeg_error_mgr pub;
  jmp_buf escape;
  char message[JMSG_LENGTH_MAX]{};
};

void jpegErrorExit(j_common_ptr cinfo) {
  JpegErrorHandler* self = reinterpret_cast<JpegErrorHandler*>(cinfo->err);
  (*cinfo->err->format_message)(cinfo, self->message);
  longjmp(self->escape, 1);
}

// Read the whole file into memory: no streaming, no size cap (a 1200x1500 greyscale
// frame is 1.8 MB against a 2 GB heap).
bool readWholeFile(const std::string& path, std::vector<uint8_t>& out) {
  HalFile f;
  if (!Storage.openFileForRead("JPG", path, f)) {
    LOG_ERR("JPG", "Failed to open %s", path.c_str());
    return false;
  }
  const int64_t size = f.size();
  if (size <= 0) {
    LOG_ERR("JPG", "Empty file: %s", path.c_str());
    f.close();
    return false;
  }
  out.resize(static_cast<size_t>(size));
  const int32_t got = f.read(out.data(), static_cast<int32_t>(out.size()));
  f.close();
  if (got != static_cast<int32_t>(size)) {
    LOG_ERR("JPG", "Short read on %s (%d of %lld)", path.c_str(), (int)got, (long long)size);
    return false;
  }
  return true;
}

// Coarse downscale factor for the DCT-domain decode. Deliberately the SAME thresholds
// as the old chooser: this value determines scaledSrcWidth/Height and therefore the
// sampling geometry, so it must not drift.
int chooseScaleDenom(float targetScale) {
  if (targetScale <= 0.125f) return 8;
  if (targetScale <= 0.25f) return 4;
  if (targetScale <= 0.5f) return 2;
  return 1;
}

// Fixed-point 16.16 arithmetic avoids software float emulation on ESP32-C3 (no FPU).
constexpr int FP_SHIFT = 16;
constexpr int32_t FP_ONE = 1 << FP_SHIFT;
constexpr int32_t FP_MASK = FP_ONE - 1;

int jpegDrawCallback(BandBlock* pDraw) {
  JpegContext* ctx = reinterpret_cast<JpegContext*>(pDraw->pUser);
  if (!ctx || !ctx->config || !ctx->renderer) return 0;

  // In EIGHT_BIT_GRAYSCALE mode, pPixels contains 8-bit grayscale values
  // Buffer is densely packed: stride = pDraw->iWidth, valid columns = pDraw->iWidthUsed
  uint8_t* pixels = reinterpret_cast<uint8_t*>(pDraw->pPixels);
  const int stride = pDraw->iWidth;
  const int validW = pDraw->iWidthUsed;
  const int blockH = pDraw->iHeight;

  if (stride <= 0 || blockH <= 0 || validW <= 0) return 1;

  const bool useDithering = ctx->config->useDithering;
  const int32_t fineScaleFPX = ctx->fineScaleFPX;
  const int32_t invScaleFPX = ctx->invScaleFPX;
  const int32_t fineScaleFPY = ctx->fineScaleFPY;
  const int32_t invScaleFPY = ctx->invScaleFPY;
  GfxRenderer& renderer = *ctx->renderer;
  const int cfgX = ctx->config->x;
  const int cfgY = ctx->config->y;
  const int blockX = pDraw->x;
  const int blockY = pDraw->y;

  // Determine destination pixel range covered by this source block
  const int srcYEnd = blockY + blockH;
  const int srcXEnd = blockX + validW;

  int dstYStart = (int)((int64_t)blockY * fineScaleFPY >> FP_SHIFT);
  int dstYEnd = (srcYEnd >= ctx->scaledSrcHeight) ? ctx->dstHeight : (int)((int64_t)srcYEnd * fineScaleFPY >> FP_SHIFT);
  int dstXStart = (int)((int64_t)blockX * fineScaleFPX >> FP_SHIFT);
  int dstXEnd = (srcXEnd >= ctx->scaledSrcWidth) ? ctx->dstWidth : (int)((int64_t)srcXEnd * fineScaleFPX >> FP_SHIFT);

  // Pre-clamp destination ranges to screen bounds (eliminates per-pixel screen checks)
  int clampYMax = ctx->dstHeight;
  if (ctx->screenHeight - cfgY < clampYMax) clampYMax = ctx->screenHeight - cfgY;
  if (dstYStart < -cfgY) dstYStart = -cfgY;
  if (dstYEnd > clampYMax) dstYEnd = clampYMax;

  int clampXMax = ctx->dstWidth;
  if (ctx->screenWidth - cfgX < clampXMax) clampXMax = ctx->screenWidth - cfgX;
  if (dstXStart < -cfgX) dstXStart = -cfgX;
  if (dstXEnd > clampXMax) dstXEnd = clampXMax;

  if (dstYStart >= dstYEnd || dstXStart >= dstXEnd) return 1;

  // Pre-compute orientation and render-mode state once per callback invocation
  DirectPixelWriter pw;
  pw.init(renderer);


  // === 1:1 fast path: no scaling math ===
  if (fineScaleFPX == FP_ONE && fineScaleFPY == FP_ONE) {
    for (int dstY = dstYStart; dstY < dstYEnd; dstY++) {
      const int outY = cfgY + dstY;
      pw.beginRow(outY);
      const uint8_t* row = &pixels[(dstY - blockY) * stride];
      for (int dstX = dstXStart; dstX < dstXEnd; dstX++) {
        const int outX = cfgX + dstX;
        uint8_t gray = row[dstX - blockX];
        const uint8_t dithered = ctx->dither ? (*ctx->dither)(gray, outX, outY)

                                                : static_cast<uint8_t>(gray / 85);
        pw.writePixel(outX, dithered);
      }
    }
    return 1;
  }

  // === Bilinear interpolation (upscale: fineScale > 1.0) ===
  // Smooths block boundaries that would otherwise create visible banding
  // on progressive JPEG DC-only decode (1/8 resolution upscaled to target).
  if (fineScaleFPX > FP_ONE && fineScaleFPY > FP_ONE) {
    // Pre-compute safe X range where lx0 and lx0+1 are both in [0, validW-1].
    // Only the left/right edge pixels (typically 0-2 and 1-8 respectively) need clamping.
    int safeXStart = (int)(((int64_t)blockX * fineScaleFPX + FP_MASK) >> FP_SHIFT);
    int safeXEnd = (int)((int64_t)(blockX + validW - 1) * fineScaleFPX >> FP_SHIFT);
    if (safeXStart < dstXStart) safeXStart = dstXStart;
    if (safeXEnd > dstXEnd) safeXEnd = dstXEnd;
    if (safeXStart > safeXEnd) safeXEnd = safeXStart;

    for (int dstY = dstYStart; dstY < dstYEnd; dstY++) {
      const int outY = cfgY + dstY;
      pw.beginRow(outY);
      const int32_t srcFyFP = dstY * invScaleFPY;
      const int32_t fy = srcFyFP & FP_MASK;
      const int32_t fyInv = FP_ONE - fy;
      int ly0 = (srcFyFP >> FP_SHIFT) - blockY;
      int ly1 = ly0 + 1;
      if (ly0 < 0) ly0 = 0;
      if (ly0 >= blockH) ly0 = blockH - 1;
      if (ly1 >= blockH) ly1 = blockH - 1;

      const uint8_t* row0 = &pixels[ly0 * stride];
      const uint8_t* row1 = &pixels[ly1 * stride];

      // Left edge (with X boundary clamping)
      for (int dstX = dstXStart; dstX < safeXStart; dstX++) {
        const int outX = cfgX + dstX;
        const int32_t srcFxFP = dstX * invScaleFPX;
        const int32_t fx = srcFxFP & FP_MASK;
        const int32_t fxInv = FP_ONE - fx;
        int lx0 = (srcFxFP >> FP_SHIFT) - blockX;
        int lx1 = lx0 + 1;
        if (lx0 < 0) lx0 = 0;
        if (lx1 < 0) lx1 = 0;
        if (lx0 >= validW) lx0 = validW - 1;
        if (lx1 >= validW) lx1 = validW - 1;

        int top = ((int)row0[lx0] * fxInv + (int)row0[lx1] * fx) >> FP_SHIFT;
        int bot = ((int)row1[lx0] * fxInv + (int)row1[lx1] * fx) >> FP_SHIFT;
        uint8_t gray = (uint8_t)((top * fyInv + bot * fy) >> FP_SHIFT);

        const uint8_t dithered = ctx->dither ? (*ctx->dither)(gray, outX, outY)


                                                : static_cast<uint8_t>(gray / 85);
        pw.writePixel(outX, dithered);
      }

      // Interior (no X boundary checks — lx0 and lx0+1 guaranteed in bounds)
      for (int dstX = safeXStart; dstX < safeXEnd; dstX++) {
        const int outX = cfgX + dstX;
        const int32_t srcFxFP = dstX * invScaleFPX;
        const int32_t fx = srcFxFP & FP_MASK;
        const int32_t fxInv = FP_ONE - fx;
        const int lx0 = (srcFxFP >> FP_SHIFT) - blockX;

        int top = ((int)row0[lx0] * fxInv + (int)row0[lx0 + 1] * fx) >> FP_SHIFT;
        int bot = ((int)row1[lx0] * fxInv + (int)row1[lx0 + 1] * fx) >> FP_SHIFT;
        uint8_t gray = (uint8_t)((top * fyInv + bot * fy) >> FP_SHIFT);

        const uint8_t dithered = ctx->dither ? (*ctx->dither)(gray, outX, outY)


                                                : static_cast<uint8_t>(gray / 85);
        pw.writePixel(outX, dithered);
      }

      // Right edge (with X boundary clamping)
      for (int dstX = safeXEnd; dstX < dstXEnd; dstX++) {
        const int outX = cfgX + dstX;
        const int32_t srcFxFP = dstX * invScaleFPX;
        const int32_t fx = srcFxFP & FP_MASK;
        const int32_t fxInv = FP_ONE - fx;
        int lx0 = (srcFxFP >> FP_SHIFT) - blockX;
        int lx1 = lx0 + 1;
        if (lx0 >= validW) lx0 = validW - 1;
        if (lx1 >= validW) lx1 = validW - 1;

        int top = ((int)row0[lx0] * fxInv + (int)row0[lx1] * fx) >> FP_SHIFT;
        int bot = ((int)row1[lx0] * fxInv + (int)row1[lx1] * fx) >> FP_SHIFT;
        uint8_t gray = (uint8_t)((top * fyInv + bot * fy) >> FP_SHIFT);

        const uint8_t dithered = ctx->dither ? (*ctx->dither)(gray, outX, outY)


                                                : static_cast<uint8_t>(gray / 85);
        pw.writePixel(outX, dithered);
      }
    }
    return 1;
  }

  // === Nearest-neighbor (downscale: fineScale < 1.0) ===
  for (int dstY = dstYStart; dstY < dstYEnd; dstY++) {
    const int outY = cfgY + dstY;
    pw.beginRow(outY);
    const int32_t srcFyFP = dstY * invScaleFPY;
    int ly = (srcFyFP >> FP_SHIFT) - blockY;
    if (ly < 0) ly = 0;
    if (ly >= blockH) ly = blockH - 1;
    const uint8_t* row = &pixels[ly * stride];

    for (int dstX = dstXStart; dstX < dstXEnd; dstX++) {
      const int outX = cfgX + dstX;
      const int32_t srcFxFP = dstX * invScaleFPX;
      int lx = (srcFxFP >> FP_SHIFT) - blockX;
      if (lx < 0) lx = 0;
      if (lx >= validW) lx = validW - 1;
      uint8_t gray = row[lx];

      const uint8_t dithered = ctx->dither ? (*ctx->dither)(gray, outX, outY)


                                              : static_cast<uint8_t>(gray / 85);
      pw.writePixel(outX, dithered);
    }
  }

  return 1;
}

}  // namespace

bool JpegToFramebufferConverter::getDimensionsStatic(const std::string& imagePath, ImageDimensions& out) {
  std::vector<uint8_t> file;
  if (!readWholeFile(imagePath, file)) return false;

  JpegErrorHandler err;
  jpeg_decompress_struct cinfo;
  std::memset(&cinfo, 0, sizeof(cinfo));
  bool created = false;
  cinfo.err = jpeg_std_error(&err.pub);
  err.pub.error_exit = jpegErrorExit;
  if (setjmp(err.escape)) {
    if (created) jpeg_destroy_decompress(&cinfo);
    LOG_ERR("JPG", "Bad JPEG header: %s (%s)", imagePath.c_str(), err.message);
    return false;
  }

  jpeg_create_decompress(&cinfo);
  created = true;
  jpeg_mem_src(&cinfo, file.data(), static_cast<unsigned long>(file.size()));
  if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
    jpeg_destroy_decompress(&cinfo);
    LOG_ERR("JPG", "Not a JPEG: %s", imagePath.c_str());
    return false;
  }

  out.width = static_cast<int>(cinfo.image_width);
  out.height = static_cast<int>(cinfo.image_height);
  jpeg_destroy_decompress(&cinfo);
  LOG_DBG("JPG", "Image dimensions: %dx%d", out.width, out.height);

  return true;
}

bool JpegToFramebufferConverter::decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer,
                                                     const RenderConfig& config) {
  LOG_DBG("JPG", "Decoding JPEG: %s", imagePath.c_str());

  JpegContext ctx;
  ctx.renderer = &renderer;
  ctx.config = &config;
  ctx.dither = std::make_unique<ko::ImageDitherer>(ko::imageDitherOptions());
  ctx.dither->reset(config.maxWidth > 0 ? config.maxWidth : renderer.getScreenWidth(), config.x, config.y);
  ctx.screenWidth = renderer.getScreenWidth();
  ctx.screenHeight = renderer.getScreenHeight();

  auto& perf = ko::imagePerf();
  perf.images += 1;
  perf.decodes += 1;          // one decode per call: a page showing an image N times decodes it N times

  const double tRead = ko::perfNowMs();
  std::vector<uint8_t> file;
  if (!readWholeFile(imagePath, file)) return false;
  perf.readMs += ko::perfNowMs() - tRead;

  JpegErrorHandler err;
  jpeg_decompress_struct cinfo;
  std::memset(&cinfo, 0, sizeof(cinfo));
  bool created = false;
  cinfo.err = jpeg_std_error(&err.pub);
  err.pub.error_exit = jpegErrorExit;
  if (setjmp(err.escape)) {
    if (created) jpeg_destroy_decompress(&cinfo);
    LOG_ERR("JPG", "Decode failed on %s: %s", imagePath.c_str(), err.message);
    return false;
  }

  jpeg_create_decompress(&cinfo);
  created = true;
  const double tHeader = ko::perfNowMs();
  jpeg_mem_src(&cinfo, file.data(), static_cast<unsigned long>(file.size()));
  if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
    jpeg_destroy_decompress(&cinfo);
    LOG_ERR("JPG", "Bad JPEG header: %s", imagePath.c_str());
    return false;
  }
  perf.headerMs += ko::perfNowMs() - tHeader;

  const int srcWidth = static_cast<int>(cinfo.image_width);
  const int srcHeight = static_cast<int>(cinfo.image_height);

  if (srcWidth <= 0 || srcHeight <= 0) {
    jpeg_destroy_decompress(&cinfo);
    LOG_ERR("JPG", "Invalid JPEG dimensions: %dx%d", srcWidth, srcHeight);
    return false;
  }

  if (!validateImageDimensions(srcWidth, srcHeight, "JPEG")) {
    jpeg_destroy_decompress(&cinfo);
    return false;
  }

  // Progressive files now decode at FULL quality: the old "DC coefficients only"
  // path was a decoder limitation (an 8x blur), not a requirement.
  const bool isProgressive = cinfo.progressive_mode != 0;

  // Calculate overall target scale
  float targetScale;
  int destWidth, destHeight;

  if (config.useExactDimensions && config.maxWidth > 0 && config.maxHeight > 0) {
    destWidth = config.maxWidth;
    destHeight = config.maxHeight;
    targetScale = (float)destWidth / srcWidth;
  } else {
    float scaleX = (config.maxWidth > 0 && srcWidth > config.maxWidth) ? (float)config.maxWidth / srcWidth : 1.0f;
    float scaleY = (config.maxHeight > 0 && srcHeight > config.maxHeight) ? (float)config.maxHeight / srcHeight : 1.0f;
    targetScale = (scaleX < scaleY) ? scaleX : scaleY;
    if (targetScale > 1.0f) targetScale = 1.0f;

    destWidth = (int)(srcWidth * targetScale);
    destHeight = (int)(srcHeight * targetScale);
  }

  // Coarse DCT-domain downscale. Progressive files are deliberately NOT forced to 1/8
  // any more: that existed only because JPEGDEC decodes progressive DC-only, throwing
  // away 7/8 of the detail. The coarse factor now comes from the target scale for both
  // kinds of file, so the resampler is fed full-quality pixels. The destination box is
  // identical either way.
  const int jpegScaleDenom = chooseScaleDenom(targetScale);

  if (destWidth <= 0 || destHeight <= 0) {
    LOG_ERR("JPG", "Degenerate output dimensions %dx%d for %s, skipping render", destWidth, destHeight,
            imagePath.c_str());
    return false;
  }

  ctx.scaledSrcWidth = (srcWidth + jpegScaleDenom - 1) / jpegScaleDenom;
  ctx.scaledSrcHeight = (srcHeight + jpegScaleDenom - 1) / jpegScaleDenom;
  ctx.dstWidth = destWidth;
  ctx.dstHeight = destHeight;
  ctx.fineScaleFPX = (int32_t)((int64_t)destWidth * FP_ONE / ctx.scaledSrcWidth);
  ctx.invScaleFPX = (int32_t)((int64_t)ctx.scaledSrcWidth * FP_ONE / destWidth);
  ctx.fineScaleFPY = (int32_t)((int64_t)destHeight * FP_ONE / ctx.scaledSrcHeight);
  ctx.invScaleFPY = (int32_t)((int64_t)ctx.scaledSrcHeight * FP_ONE / destHeight);

  LOG_DBG("JPG", "JPEG %dx%d -> %dx%d (scale %.2f, jpegScale 1/%d, fineScale %.2f)%s", srcWidth, srcHeight, destWidth,
          destHeight, targetScale, jpegScaleDenom, (float)destWidth / ctx.scaledSrcWidth,
          isProgressive ? " [progressive]" : "");

  // Decode the whole frame once, greyscale, with the integer ISLOW IDCT so host and
  // wasm produce identical pixels (SIMD is disabled in both builds).
  cinfo.out_color_space = JCS_GRAYSCALE;
  cinfo.dct_method = JDCT_ISLOW;
  cinfo.scale_num = 1;
  cinfo.scale_denom = static_cast<unsigned int>(jpegScaleDenom);
  jpeg_start_decompress(&cinfo);

  const int decodedW = static_cast<int>(cinfo.output_width);
  const int decodedH = static_cast<int>(cinfo.output_height);
  // The geometry math assumes ceil(srcW / denom). libjpeg rounds scaled output the same
  // way, but verify rather than assume: a mismatch would silently shift every sample.
  if (decodedW != ctx.scaledSrcWidth || decodedH != ctx.scaledSrcHeight) {
    LOG_ERR("JPG", "Scaled dimension mismatch: decoder %dx%d, geometry %dx%d", decodedW, decodedH,
            ctx.scaledSrcWidth, ctx.scaledSrcHeight);
    jpeg_destroy_decompress(&cinfo);
    return false;
  }

  // THE CODEC'S OWN WORK, measured where it happens. The previous timer here was named decode but
  // started after this loop, so it reported draw time under a decode label.
  const double tDecode = ko::perfNowMs();
  std::vector<uint8_t> frame(static_cast<size_t>(decodedW) * static_cast<size_t>(decodedH));
  while (cinfo.output_scanline < cinfo.output_height) {
    JSAMPROW rows[1] = {frame.data() + static_cast<size_t>(cinfo.output_scanline) * decodedW};
    jpeg_read_scanlines(&cinfo, rows, 1);
  }
  jpeg_finish_decompress(&cinfo);
  jpeg_destroy_decompress(&cinfo);
  created = false;
  perf.decodeMs += ko::perfNowMs() - tDecode;

  // Hand the whole frame to the unchanged band callback as one band: scale + dither + writes.
  const double tDraw = ko::perfNowMs();
  BandBlock draw{};
  draw.pUser = &ctx;
  draw.x = 0;
  draw.y = 0;
  draw.iWidth = decodedW;
  draw.iWidthUsed = decodedW;
  draw.iHeight = decodedH;
  draw.pPixels = reinterpret_cast<uint16_t*>(frame.data());
  jpegDrawCallback(&draw);
  perf.drawMs += ko::perfNowMs() - tDraw;
  LOG_DBG("JPG", "JPEG complete - render time: %.2f ms", perf.drawMs);

  return true;
}

bool JpegToFramebufferConverter::supportsFormat(const std::string& extension) {
  return FsHelpers::hasJpgExtension(extension);
}
