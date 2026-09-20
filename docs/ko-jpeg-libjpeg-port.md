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
   - `readWholeFile()` → `jpeg_mem_src()`, with the 128 MiB compressed-file ceiling enforced before
     the vector allocation (and the same ceiling applied to the ZIP member before extraction).
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


## Next: the cover path (this is what still blocks JPEGDEC removal)

JPEGDEC cannot be deleted until the cover converters stop using it:

- `vendor-lib/JpegToBmpConverter/JpegToBmpConverter.cpp` (733 lines) - used by
  `Epub.cpp` for covers (line ~628) and thumbnails (~725), and by `Txt.cpp` (~142)
- `vendor-lib/PngToBmpConverter/PngToBmpConverter.cpp` (845 lines) - `Epub.cpp` (~662)

Same shape as the framebuffer port: read the whole file, decode the whole frame with
libjpeg (`JCS_GRAYSCALE`, `JDCT_ISLOW`) / libpng, and replace the MCU draw callback
with a direct row loop. Keep the BMP writer, the crop math and the 1-bit output path
untouched; drop the heap gates; stop forcing progressive to 1/8. Then remove
`third_party/JPEGDEC` and `third_party/PNGdec` from CMake and the include path.

Gate: cover BMP bytes identical for baseline images (and strictly better for
progressive, which currently arrives as a 1/8 blur).

## Dither comparison on the now-undegraded source

`prog_panel` (the progressive image, full decode), local tone error / detail / grain:

| candidate | tone | detail | grain |
|---|---|---|---|
| blue noise (kofork) - SHIPPED | 1.372 | 0.775 | 1043 |
| kohash (fork hash dither) | 3.102 | 0.778 | 1022 |
| fs (pr1614) | 4.067 | 0.828 | 780 |
| legacy Bayer +/-40 - PREVIOUS | 9.734 | 0.949 | 6.3 |
| all *_master variants | 11.6-13.7 | 0.91-0.94 | <=11 |

The `master` profile anomaly reproduced on a third independent source (11.6 vs 1.37),
which is why the default is the ko fork triple. Read tone WITH detail/grain: legacy and
master win detail and grain precisely because they band.


## DONE: JPEGDEC removed

Both JPEG converters (framebuffer + cover) decode with libjpeg-turbo. JPEGDEC survived
only as the JPEGDRAW struct, so it was replaced by a local `BandBlock.h` and dropped
from CMake - 0 references in the build, verified on host AND wasm.

Verified after removal: 2048 pages, 0 differing pages, 0 differing pixels.

NOT verified: end-to-end cover rendering. Covers are library-UI only and our host
harness never renders them, so the cover converter compiles and its geometry code is
untouched, but its decoded input changed (JPEGDEC -> libjpeg ISLOW), which will differ by
up to ~1 LSB from IDCT rounding. Worth a visual check on device/browser.

## REMAINING: PNGdec -> libpng

`third_party/PNGdec` is still in use by BOTH PNG converters:
- `Epub/Epub/converters/PngToFramebufferConverter.cpp` (~437 lines, framebuffer path)
- `PngToBmpConverter/PngToBmpConverter.cpp` (845 lines, cover path)

Same recipe: vendor libpng + zlib (ExternalProject, SIMD off, one source for host and
wasm), whole-file read, whole-frame decode, feed the existing callback via a synthetic
BandBlock (the band-block struct is already shared, so no new plumbing is needed).
Then remove PNGdec from CMake.


## 1-bit blue-noise dither: KEPT AS IS (decision 2026-09-17)

User reported 1-bit XTG text looking "a bit too thick black". Measured before changing
anything, on 4 text pages of demo.epub (dither ON vs OFF vs the 2-bit reference):

| page | 2-bit implied ink mass | 1-bit dither | 1-bit old threshold |
|------|------------------------|--------------|---------------------|
|   20 | 10.64%                 | 10.61%       | 11.60%              |
|  120 | 12.58%                 | 12.51%       | 13.71%              |
|  500 |  9.24%                 |  9.18%       | 10.09%              |
| 1000 | 10.70%                 | 10.64%       | 11.69%              |

Empirical density actually applied (page 120): v=1 0.916 (nominal 0.922), v=2 0.658
(0.667), v=3 0.997 (1.0). Implementation does what it claims.

CONCLUSIONS - the dither is NOT what made it heavy. It matches the 4-level reference to
within 0.1% and is ~9% LIGHTER than the old hard threshold. Two real causes remain:

1. mono/core = 1.50x. The solid-black core is only 7-8% of a text page; the AA band
   (v=1,2) adds ~50% on top. In the 2-bit page those pixels are grey; in 1-bit they are
   dithered near-black dots ATTACHED to the stroke, and on 2-3 px strokes there is no
   area for the eye to average over. Dot gain plus zero averaging area - intrinsic.
2. ROOT CAUSE: the reference is bold by construction. The font pack thresholds in
   tools/ttf_to_epdfont_fast.py quantize coverage >=25% -> dark grey, >=50% -> light
   grey, >=75% -> BLACK. A 25% edge pixel is already dark grey, so faithful dithering
   reproduces a heavy look.

Calibration curve if this is ever revisited (ink mass, page 120, no rebuild needed):
  reference 12.58% | current(235/170) 12.57% | x0.80 11.74% | x0.65 11.12%
  | x0.50 10.49% | PR#2179(160/48) 10.49%

If it IS revisited: raise the FONT coverage thresholds (helps both output modes, but
diverges from the stock font compilation) rather than only lightening the densities.
Best done alongside a real test pattern from the device, because the panel anchors
15/30/80/210 used to derive the densities are still unverified.
