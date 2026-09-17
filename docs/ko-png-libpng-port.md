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

---

## Stage 2 mechanics settled (2026-09-17, second pass)

**Must be atomic.** PNGdec's `iPixelType` is an enum type taken BY TYPE by
`convertLineToGray`, so defining our own `PNG_PIXEL_*` constants while `PNGdec.h` is still
included is an enumerator clash, and the call site stops type-checking:

    BandBlock.h:35: error: redefinition of enumerator 'PNG_PIXEL_GRAYSCALE'
    PngToFramebufferConverter.cpp:231: error: no matching function for call to 'convertLineToGray'

So: shared constants + struct + `PNGdec.h` removal + decode swap happen in ONE step.

Ready-made script: `scripts/png-port/stage2b_atomic.py`. It does not rewrite the ~100-line
region; it deletes only specific constructs (brace-counted, with span-length assertions so
a bad anchor aborts instead of eating code) and swaps the decode call:

1. heap gates (`ESP.getMaxAllocHeap` + `MIN_FREE_HEAP_FOR_PNG`) - both occurrences
2. `unique_ptr<PNG>` allocation
3. `png->open(...)` + its `ScopedCleanup`
4. `PNG_SUCCESS` -> 0
5. PNGdec's two-scanline overflow guard
6. the file callbacks `pngOpenWithHandle` .. `pngSeekWithHandle` (asserts span <= 60 lines
   so `convertLineToGray` cannot be swallowed)
7. `#include <PNGdec.h>` out, `BandBlock.h` in
8. `PNGDRAW` -> `PngBlock` (keeps every field name, so the callback body is untouched)
9. `validateImageDimensions(png->getWidth(), png->getHeight(), ...)` -> whole-file read +
   libpng dimensions; `ctx.srcWidth/srcHeight` -> those; `rc = png->decode(&ctx, 0)` ->
   row loop calling `pngDrawCallback` per row with a synthetic `PngBlock`

### MISSING PIECE - write this first

The script calls a helper that does not exist yet. Add it to `BandBlock.h` (or a new
`PngDecodeSession.h`) and include it:

    class PngDecodeSession {
     public:
      bool begin(const std::vector<uint8_t>& file, const std::string& path, int w, int h);
      bool hasAlpha() const;     // post-update_info colour type has an alpha channel
      int passes() const;        // png_set_interlace_handling result (1 if not interlaced)
      void readRow(uint8_t* dst);  // png_read_row
      ~PngDecodeSession();         // png_destroy_read_struct
    };
    // plus: int pngReadDimensions(const std::vector<uint8_t>& file, const std::string& path);
    //       int pngReadHeight(...);

Normalisation inside `begin()`: `png_read_info` -> `png_set_strip_16` if 16-bit ->
`png_set_palette_to_rgb` if palette -> `png_set_expand_gray_1_2_4_to_8` if low-bit grey ->
`png_set_rgb_to_gray_fixed` for RGB/palette -> `png_set_tRNS_to_alpha` if tRNS ->
`png_set_interlace_handling` -> `png_read_update_info`. Do NOT strip alpha: the callback's
`convertLineToGray` handles GRAY_ALPHA itself and the old path passed `iHasAlpha` too, so
stripping would change compositing and break the gate. Errors via
`setjmp(png_jmpbuf(png))` so a malformed file fails the image, not the process.

Gate (baseline already captured): `/tmp/ab/png_before.xtch`, 1345804 B / 14 pages -
page-level identical, 0 differing pixels. PNG is lossless, so identical is the target.

---

## Stage 2 - CLOSED (2026-09-17)

The framebuffer converter's scanline decode now uses libpng. Design C: PNGdec's PNGDRAW is
kept as the callback parameter and filled synthetically, exactly as the JPEG converters keep
JPEGDRAW. The callback is already per-scanline and does its own row mapping, so its body is
untouched. PNGdec.h stays until stage 3 removes it with the cover converter, because its
iPixelType is an enum type taken by type by convertLineToGray.

### Two device-era constraints found and removed

1. A code-level cap: the gray scratch row was bounded by PNG_MAX_BUFFERED_PIXELS/2 = 1281, so
   any image wider than that failed outright. Nothing in this port has a fixed row buffer.
2. PNGdec's own hard internal buffer: even with (1) removed, PNGdec reports
   "PNG row buffer too small: need 3648 bytes for width=1807 type=0 bpp=8, configured
   PNG_MAX_BUFFERED_PIXELS=2562" and aborts. wide_scaling_test.png (1807x736) was therefore
   NEVER rendered by the old path - a real, pre-existing defect that this port fixes.

### Gate - restated honestly

Comparing decoders with constraint (1) removed in both, so the decoders are compared rather
than the cap:

- 12 of 14 pages: **byte-identical, 0 differing pixels**.
- The 2 differing pages are entirely the 1807x736 image that PNGdec cannot decode: page 7
  diff (3556 px) and page 8 diff (25834 px) both fall inside that image's rect, and nothing
  else on either page moved.

Equivalence holds everywhere PNGdec could decode; where it could not, the port is strictly
better. Host and wasm both build (wasm exit 0).

---

## Stage 3 - CLOSED (2026-09-17): PNGdec is gone

**The handoff was wrong about this one.** The cover converter
(vendor-lib/PngToBmpConverter/PngToBmpConverter.cpp) never used PNGdec: it is a
self-contained decoder with its own chunk walker (findNextIdatChunk), scanline decoder
(decodeScanline), grey conversion (convertScanlineToGray) and inflate. So stage 3 was a
DELETION, not a port - no cover-code changes were needed at all.

PNGdec was referenced by exactly two files: CMakeLists.txt and the framebuffer converter.

### What was done

1. BandBlock.h gains PNGdec-compatible definitions: PNG_PIXEL_* enumerators, a PNGDRAW
   struct with the same field names, and PNG_SUCCESS = 0. The framebuffer callback body is
   therefore untouched by the removal.
2. The framebuffer converter drops <PNGdec.h> for BandBlock.h.
3. 29 lines of dead PNGdec file callbacks deleted (pngOpenWithHandle..pngSeekWithHandle,
   the PNGFILE users).
4. One call-site cast: the synthetic block's pPalette is void* while convertLineToGray
   declares uint8_t* - void* does not implicitly convert to uint8_t* in C++.
5. CMakeLists.txt: 8 PNGdec lines removed (include dir + 7 sources, including its bundled
   zlib). third_party/PNGdec deleted.

### Gate

The PNGdec-free build is **byte-identical** to the design-C build: demo-png.epub, 14 pages,
**0 differing pages, 0 differing pixels**. Host builds; wasm builds (exit 0).

### Result

No device-era decoder remains in the tree: JPEG on libjpeg-turbo, PNG on libpng, PXC cache
gone, MCU band machinery gone.
