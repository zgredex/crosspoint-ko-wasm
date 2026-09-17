# JPEG decoder port: JPEGDEC → libjpeg-turbo

Status: **implemented, verified, then lost to a bad patch — this file is the redo recipe.**

## Why

`third_party/JPEGDEC` is an MCU decoder (bitbank2): banded MCU callbacks, fixed internal
buffers, a ~20 KB decoder object, and a heap gate. Its progressive path is documented in the
converter itself:

    // on progressive JPEG DC-only decode (1/8 resolution upscaled to target)

Measured cost of that path (full decode vs DC-only 1/8, same target box):

    mean |full - dc_only| : 21.76 grey levels
    RMSE vs full decode   : 51.91 grey levels

~20% of full scale = an 8x blur. Our test books only carry baseline JPEGs, so the defect was
latent — but web-sourced EPUBs ship progressive routinely.

Also measured: libjpeg-turbo decodes 1200x1500 in **1.49 ms**, progressive full-res in
**0.64 ms**, and DCT-domain scaling to 1/2, 1/4, 1/8 all work.

## What was done (and verified)

1. **Vendored** libjpeg-turbo 3.0.4 → `third_party/libjpeg-turbo`.
2. **CMake**: upstream *refuses* `add_subdirectory()` (hard `FATAL_ERROR`), so it is built via
   `ExternalProject_Add` and imported as a static `jpeg-static` target:
   `ENABLE_SHARED=OFF ENABLE_STATIC=ON WITH_SIMD=OFF WITH_TURBOJPEG=OFF WITH_12BIT=OFF
   WITH_JAVA=OFF`, forwarding `CMAKE_TOOLCHAIN_FILE` so one source tree serves host and wasm.
   SIMD is off deliberately: SIMD IDCT is not guaranteed bit-identical to C ISLOW, and the host
   build is the byte-exact verification harness. Our decoder pins `dct_method = JDCT_ISLOW`.
3. **Converter rewrite** (`vendor-lib/Epub/Epub/converters/JpegToFramebufferConverter.cpp`):
   - `readWholeFile()` → `jpeg_mem_src()`. No streaming, no size cap.
   - `JpegErrorHandler` with `setjmp`/`longjmp`: libjpeg's default `error_exit` is `exit()`,
     which in wasm would kill the whole conversion. Corrupt file = fail that image only.
   - `chooseScaleDenom(targetScale)` keeps the **same thresholds** as the old chooser
     (0.125→8, 0.25→4, 0.5→2, else 1) because the value determines `scaledSrcW/H` and therefore
     the sampling geometry.
   - **Progressive is no longer forced to 1/8** — it now selects from `targetScale` like
     baseline, so the source feeding the resampler is full quality.
   - Decode whole frame as `JCS_GRAYSCALE`, `JDCT_ISLOW`, `scale_num=1`,
     `scale_denom=jpegScaleDenom`; assert `output_width/height` equal `scaledSrcWidth/Height`
     (the geometry math assumes `ceil(w/denom)`).
   - The old callback `jpegDrawCallback(JPEGDRAW*)` became
     `drawBand(JpegContext*, uint8_t* pixels, int stride, int validW, int blockX, int blockY,
     int blockH)` and is called **once** with the whole frame `(0, 0, w, w, 0, 0, h)`.
     The three sampling paths are block-relative (`- blockX`, `- blockY`), so a full-frame band
     makes those the identity: **the mapping is unchanged and the box cannot drift.**
   - Dropped: the device heap gate, the MCU band plumbing, the `JPEG_DECODER_APPROX_SIZE`
     constants, `jpegOpen/Close/Read/Seek`.
4. **Verified**: output sizes byte-identical to the pre-swap baselines —
   `demo-images.epub` **1,345,804 B**, `demo.epub` **195,347,364 B** (2,034 pages / 60 chapters).
   Geometry pinned, as required.

## What lost it

A `patch` call with `replace_all=true` on a bare statement (`if (caching) cw.writePixel(...)`)
had its fuzzy matcher mis-fire and delete ~100 unrelated lines across the whole file. No backup
existed and the file was uncommitted, so it was restored from the initial commit and the libjpeg
work must be redone.

**Lesson: commit before a multi-edit pass, and never `replace_all` a bare statement — include
enough surrounding context to make the match unique.**

## Remaining work

- Redo the converter rewrite above (the recipe is complete here).
- **Delete the PXC cache** (decision: user confirmed it is device-only, no technical constraints
  remain). It never activated in our builds — `HalStorage` is a stub over an in-memory map, so no
  `.pxc` is ever written or found — therefore removal must be **byte-identical on output** (that
  is the gate). Sites:
  - `PixelCache.h` (+ `DirectCacheWriter`), `JpegToFramebufferConverter.{h,cpp}` and
    `PngToFramebufferConverter.cpp` (`cache`/`caching` members, per-pixel `cw.writePixel`, band
    setup), `ImageBlock.cpp` (`getCachePath`, `readValidCacheHeader`, `imagePathHash`, the
    `pxcChunks[6]` slot, `loadPxcSlot`, `renderRowsFromPxcSlot`, `renderFromCache` and its call
    site), and `RenderConfig::cachePath`.
  - Device constants that only exist for the cache: `PXC_CHUNK_SHIFT/MAX_CHUNKS/HEAP_RESERVE/
    MAX_ALLOC_RESERVE/MAX_BYTES_PER_ROW`.
- **PNG → libpng** (same class of win; PNGdec has palette/buffer caps).
- **Progressive EPUB test** end-to-end.
- **Re-run the dither comparison** now that decode no longer degrades the source.
- JPEGDEC removal: blocked on `vendor-lib/JpegToBmpConverter` (still uses it).
