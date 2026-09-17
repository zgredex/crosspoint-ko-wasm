#!/usr/bin/env python3
"""PNG stage 2 (design C): decode the rows with libpng, feed the UNCHANGED callback.

Why not the earlier "atomic struct swap" design: PNGdec's iPixelType is an enum type taken
by type by convertLineToGray, so our own enumerators clash with PNGdec's while its header
is included. Keeping PNGdec's PNGDRAW struct as the callback's parameter - and filling it
synthetically, exactly as the JPEG converters keep JPEGDRAW - sidesteps that entirely.
PNGdec.h therefore stays for now; stage 3 removes it together with the cover converter.

Deliberately NOT touched here: the heap gates. They reference MIN_FREE_HEAP_FOR_PNG, which
is defined in terms of a PNGdec constant, so they can only go once that header goes. They
are inert on host/wasm anyway (heap is large). Walking up to \"the nearest if (\" to delete
them would anchor on the wrong block, because maxAlloc is declared *before* its if.

Edits are anchored on exact text or brace-counted blocks with span assertions.
"""
import re
import shutil
import sys

P = '/Users/patryk/krxtc/ko-wasm/vendor-lib/Epub/Epub/converters/PngToFramebufferConverter.cpp'
shutil.copy(P, '/tmp/ab/PngToFramebufferConverter.pre_2c')
lines = open(P).read().split('\n')
log = []


def find(pred, what, start=0):
    for i in range(start, len(lines)):
        if pred(lines[i]):
            return i
    print(f'ABORT: anchor not found: {what}')
    sys.exit(1)


def block_end(i, what, maxspan=200):
    depth = 0
    for k in range(i, min(len(lines), i + maxspan)):
        code = re.sub(r'"(?:[^"\\]|\\.)*"', '""', lines[k])
        code = re.sub(r"'(?:[^'\\]|\\.)*'", "''", code)
        depth += code.count('{') - code.count('}')
        if depth == 0 and k > i:
            return k
    print(f'ABORT: no closing brace for {what}')
    sys.exit(1)


def delete_from(i, what, maxspan=200):
    end = block_end(i, what, maxspan)
    span = end - i + 1
    if span > maxspan:
        print(f'ABORT: span {span} too large for {what}')
        sys.exit(1)
    del lines[i:end + 1]
    log.append(f'deleted {what} ({span} lines)')


# 1. the PNGdec object allocation
i = find(lambda l: 'unique_ptr<PNG> png' in l, 'decoder alloc')
delete_from(i, 'PNGdec allocation', maxspan=12)

# 2. its open() call + ScopedCleanup (the decoder is never opened now)
i = find(lambda l: 'png->open(' in l, 'png->open')
j = find(lambda l: 'png->close(); }' in l, 'cleanup', i)
if j - i > 6:
    print('ABORT: cleanup anchor implausibly far from open')
    sys.exit(1)
del lines[i:j + 1]
log.append(f'deleted png->open + cleanup ({j - i + 1} lines)')

# 3. dimensions from libpng (inline header probe), replacing the PNGdec getters
old = 'if (!validateImageDimensions(png->getWidth(), png->getHeight(), "PNG")) {'
if '\n'.join(lines).count(old) != 1:
    print('ABORT: validate call not found verbatim')
    sys.exit(1)
new = '''  // Whole-file read, then a header-only libpng probe for the dimensions. Dimensions feed
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

  if (!validateImageDimensions(pngW, pngH, "PNG")) {'''
lines = '\n'.join(lines).replace(old, new, 1).split('\n')
log.append('dimension probe -> libpng (inline header-only read)')

s = '\n'.join(lines)
s = s.replace('ctx.srcWidth = png->getWidth();', 'ctx.srcWidth = pngW;')
s = s.replace('ctx.srcHeight = png->getHeight();', 'ctx.srcHeight = pngH;')

# 4. the decode itself
old_decode = 'rc = png->decode(&ctx, 0);'
if s.count(old_decode) != 1:
    print('ABORT: decode call not found verbatim')
    sys.exit(1)
new_decode = '''  // libpng, one row at a time, into the UNCHANGED callback: it is already per-scanline
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
          if (ctx.error) break;
        }
      }
      png_read_end(pngRead, nullptr);
      png_destroy_read_struct(&pngRead, &pngInfo, nullptr);
      rc = ctx.error ? 1 : 0;
    }
  }'''
s = s.replace(old_decode, new_decode, 1)
log.append('decode -> libpng rows into the unchanged callback')

# 5. PNGdec's two-scanline overflow guard (meaningless without its fixed buffer)
i = next((k for k, l in enumerate(s.split('\n'))
          if 'Aborting decode to avoid PNGdec internal buffer overflow' in l), -1)
if i >= 0:
    lines = s.split('\n')
    j = i
    while j > 0 and not lines[j].strip().startswith('if ('):
        j -= 1
    delete_from(j, 'PNGdec overflow guard', maxspan=10)
    s = '\n'.join(lines)

open(P, 'w').write(s)
print('stage 2 (design C) applied:')
for l in log:
    print('  -', l)
left = [l.strip()[:80] for l in s.split('\n') if 'png->' in l]
print('remaining png-> references:', len(left))
for l in left[:5]:
    print('   ', l)
