# PNGdec → libpng: staged port

Same discipline as the JPEG work: **one stage at a time, each with its own hard gate.**
Two of these gates are exact by construction (dimensions feed pagination; PNG is lossless),
which is why they are worth more than a visual check.

---

## Stage 1 — DONE and verified (`48dd25f`)

Framebuffer converter's **dimension probe** now uses libpng.

- Header-only probe over a whole-file buffer (`readWholeFilePng`), parsed with
  `png_create_read_struct` + `png_read_info`, errors via the canonical
  `setjmp(png_jmpbuf(png))` pattern so a malformed PNG fails the probe, not the process.
- Replaced the old path, which allocated a ~42 KB PNGdec plus its zlib state purely to
  read two integers.
- Helpers added in `PngToFramebufferConverter.cpp`: `PngMemReader`, `pngMemReadFn`,
  `readWholeFilePng`.

Gate: `demo-png.epub` before vs after — **identical 1,345,804 B, 14 pages, 0 differing
pages, 0 differing pixels.** Exact gate: dimensions feed pagination.

Host builds; wasm exit 0 and grew 6,875,402 → **6,908,861** (+33 KB), proving libpng is
really linked rather than dropped.

---

## Stage 2 — the scanline decode path (mapped, not yet done)

File: `vendor-lib/Epub/Epub/converters/PngToFramebufferConverter.cpp`

### Key discovery that makes this cheap

The callback is **per-row already** (`int srcY = pDraw->y;`) and does all up/downscale
row mapping itself. So libpng feeds it **one synthetic call per decoded row** and the
callback body is untouched — same trick as handing a whole frame to the JPEG callbacks.

### Fields the callback touches (complete set)

`y`, `pPixels`, `iPixelType`, `iBpp`, `pPalette`, `iHasAlpha` — used at lines 211 and
230 in a call to `convertLineToGray(...)`.

### Plan

1. **Extend `BandBlock.h`** with a PNG block carrying exactly those fields (keep the same
   names so only the *type* name changes at the call sites).
2. **Define the pixel-type constants** the callback compares against
   (`PNG_PIXEL_GRAYSCALE`, `PNG_PIXEL_GRAY_ALPHA`, `PNG_PIXEL_INDEXED`,
   `PNG_PIXEL_TRUECOLOR`, `PNG_PIXEL_TRUECOLOR_ALPHA`). Our own enum is fine — both
   `convertLineToGray` and the callback live in this file, so the numeric values only have
   to be self-consistent, not to match PNGdec's.
3. **Swap `PNGDRAW` → the new struct** in the callback signature.
4. **Replace lines ~387–490** (heap gate → `unique_ptr<PNG>` → `png->open(...)` →
   `validateImageDimensions` → context/scales → `grayLineBuffer` alloc → the overflow
   guard at ~451 → `png->decode(&ctx, 0)` → tail) with libpng:
   `png_read_info` → `png_set_palette_to_rgb` / `png_set_expand_gray_1_2_4_to_8` /
   `png_set_strip_16` / RGB→grey / `png_set_interlace_handling` → `png_read_update_info`
   → read row by row → build the block per row → call `pngDrawCallback`.
5. **Delete device-era code** (no hardware constraints): the two `MIN_FREE_HEAP_FOR_PNG`
   / `ESP.getMaxAllocHeap()` gates (lines ~339 and ~387), the ~42 KB decoder allocation,
   `PNGDECODER_APPROX_SIZE` (~line 82), and PNGdec's two-scanline overflow guard
   (~line 451) — libpng has no fixed internal scanline buffer, so it is meaningless.
6. **Delete the file callbacks** `pngOpenWithHandle` / `pngCloseWithHandle` /
   `pngReadWithHandle` / `pngSeekWithHandle` (~line 47 onward) and their global file
   pointer — dead once the decode is libpng. Then remove `#include <PNGdec.h>` from this
   file (nothing else in it needs it after step 3).

### Gate

`demo-png.epub` **page-level identical** to `/tmp/ab/png_before.xtch` (baseline already
captured, 1,345,804 B / 14 pages). PNG is lossless, so unlike the JPEG port there is no
quality upside to trade against — **identical is the target**, and any difference means
the colour handling is wrong, most likely RGB→grey or the palette/alpha path.

---

## Stage 3 — cover converter, then PNGdec out

1. Port `vendor-lib/PngToBmpConverter/PngToBmpConverter.cpp` (845 lines, PNGdec file API
   with its own callbacks; dims around line 580) — same synthetic-band treatment.
2. Delete PNGdec: the include dir in `CMakeLists.txt` (~line 50), its 7 sources
   (~lines 82–88, which include its bundled zlib), and `third_party/PNGdec`.
3. Gate: both toolchains build, and `demo-png.epub` stays identical.

---

## Already done, for context

libpng is wired into both toolchains (`2472b6e`): host via
`find_package(PNG REQUIRED)` → `PNG::PNG` on `koengine` (brew libpng 1.6.58 / zlib
1.2.12); wasm via `-sUSE_LIBPNG=1 -sUSE_ZLIB=1` (emscripten ports, `embuilder build
libpng zlib` ≈ 4 s).

**No vendoring here, deliberately:** JPEG implementations diverge in IDCT rounding — hence
vendored libjpeg-turbo with SIMD off — but PNG decoding is exact integer arithmetic and
inflate is deterministic, so the toolchain-native libpng is bit-identical.
