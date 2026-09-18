# Image dither models (selectable), at both tone depths

Until now the image dither was fixed: blue noise to 4 levels (2-bit XTCH), and in 1-bit XTC the
grey planes were halftoned by the writer's mask layers. Now the model is a UI choice — and it
applies at **whichever tone depth the export needs**: 4 tones for a 2-bit page, 2 tones for a
1-bit one.

Models come from `zgredex/crosspoint-pxc-converter` (`src/domain/dither.ts`); the **thresholds
stay the KO firmware's own** — the `kProfileMaster` tables, which the referenced converter also
uses as its default:

| | binning | level luminances |
|---|---|---|
| dither (error diffusion + ordered bracketing) | `{30, 50, 140}` | `{15, 30, 80, 210}` |
| hard quantize (dithering off) | `{45, 70, 140}` | — |

`{15,30,80,210}` is the panel reflectance model (`white 210 / dark grey 30 / light grey 80 /
black 15`) that the 1-bit writer's `kMonoInkDensity` derives its 92% / 67% ink densities from —
so both depths now agree about what a grey *is*. Two consequences worth stating plainly:

1. **The 2-bit default export changed.** The dither path previously used the port's `kofork`
   profile (binning `{45,70,140}`, nominal levels `{0,85,170,255}`). Measured on
   `demo-images.epub`: 9 of 14 pages differ, 212,848 differing bits of 10,752,000 (2.0%).
   The old comment justified `kofork` because master's levels "were never explained"; they are
   explained now, and using them removes a real inconsistency: with `kofork` levels, the 1-bit
   masks (which assume the panel model) over-inked every grey — see point 2.
2. **The 1-bit image route changed from two stages to one.** Before: decode → 4-level dither →
   writer halftones those greys again. That dithered an already-dithered image *and* mixed the
   two level conventions. Now: decode → the selected model → 2 tones, one pass from the source.

## What the 1-bit route change is worth (grey-heavy page, vs the 2-bit page)

`demo-images.epub` spine 5 page 1 (21.9% ink, 15.3% of the page in greys):

| 1-bit route | tone error | ink | pepper/1k |
|---|---|---|---|
| old: 4-level dither → writer mask halftone | 8.58% | 17.33% | 65.7 |
| new: single pass, blue noise | **0.68%** | 8.16% | 5.1 |

The old route inked 17.3% of the page where the 2-bit page it stood in for has 8.2% — it was
over-inking the greys roughly 2x, exactly the `kofork`-levels vs panel-model mismatch. Tone
error drops 12.6x and speckle 13x.

## Model comparison (2 tones, same page, vs the 4-level page)

`scripts/verify/plane_compare.py` in panel reflectance space, from the exported planes:

| model | tone error | ink | pepper/1k |
|---|---|---|---|
| **zhou-fang** | **0.65%** | 8.33% | 4.2 |
| **blue-noise (default)** | **0.68%** | 8.16% | 5.1 |
| bayer | 0.75% | 8.23% | 11.9 |
| floyd-steinberg | 0.98% | 7.87% | 5.7 |
| burkes | 1.03% | 7.83% | 6.0 |
| atkinson | 1.10% | 7.73% | 2.3 |
| stucki | 1.12% | 7.70% | 5.7 |
| jjn | 1.17% | 7.66% | 4.4 |
| none (hard threshold) | 1.26% | 7.72% | 0.9 |
| ko-fork hash | 1.37% | 8.10% | 7.1 (5.75% on black alone) |

Blue noise and Zhou–Fang are the most faithful to the 4-level page; Atkinson is the cleanest
(least speckle) at a slightly worse tone; the hard threshold has almost no speckle but is the
least faithful of the dithered options, which is what a threshold costs. ko-fork hash is kept
as a comparison point only — it is visibly the worst on solid black.

## The ko-fork hash entry is the firmware's own, and it reads dark by design

This one is not from the converter - it is the KO fork's own image quantizer, copied verbatim:

* `quantizeNoise` (`lib/GfxRenderer/BitmapHelpers.cpp:71` upstream) -> `koForkHashDither`, the
  2-bit path. Per pixel it hashes (x,y) and uses the top 8 bits as a random threshold, scaling
  the luma by 3 so the decision points sit at **even thirds: 85 and 170**.
* `quantize1bit` (same file) -> `koForkHashDither1Bit`, the 1-bit path: the same hash byte, but
  the threshold is `128 + (threshold - 128) / 2`, i.e. it spans 64..192 instead of 0..255.

**Why the 2-bit mode looks darker than everything else.** Even thirds are the correct decision
points for *evenly spaced* output luminances, i.e. `{0, 85, 170, 255}` - the fork's nominal
palette. The panel's four states are `{15, 30, 80, 210}`: the two greys sit near the dark end, so
the correct edges are near `{30, 50, 140}` (which is exactly what `quantizeSimple`, and this
port's dither tables, use). With the edges at 85/170, every tone from 85 upward mixes in the next
*darker* state far too often. Measured mean reflectance of a flat patch
(`scripts/verify/ko_hash_tone.py`):

| source luma | ko-hash renders | error | blue-noise renders | error |
|---|---|---|---|---|
| 60 | 25.6 | **-34** | 60.1 | +0.1 |
| 100 | 39.1 | **-61** | 99.8 | -0.2 |
| 128 | 55.5 | **-73** | 128.2 | +0.2 |
| 170 | 80.5 | **-90** | 169.9 | -0.1 |
| 200 | 126.9 | **-73** | 199.8 | -0.2 |
| 230 | 171.9 | **-58** | 210.0 | -20 |

Mean |error| over all 256 source tones: **54.0** for ko-hash vs **4.6** for blue noise (25.7 for
a hard threshold). Only pure white and the darkest band come out right. That is the whole
"darker than the others" effect, and it is inherent to the algorithm - so this mode is kept as a
fidelity/comparison point, labelled as such in the UI, not offered as a quality choice.

**The 1-bit path now uses the fork's rule too.** The first cut jittered the sample by +/-127 and
thresholded, which is *not* what the firmware does: it turned 6.6% of pure-black pixels white.
Measured on the grey-heavy page, old rule -> fork rule:

| 2-tone ko-hash | tone error | ink | pepper/1k | black-level error |
|---|---|---|---|---|
| +/-127 jitter (port invention) | 1.37% | 8.10% | 7.1 | 5.75% |
| `quantize1bit` (the firmware's) | **0.72%** | 8.32% | **2.1** | **0.87%** |
| blue-noise, for reference | 0.68% | 8.16% | 5.1 | 0.67% |

So at 2 tones the fork's rule is within 0.04% of blue noise on tone with **the least speckle of
any model**, and solids stay solid. It clips below luma 64 and above 192 to solid black/white,
which is what the firmware does and why it is so clean.

## Behaviour invariants (measured)

* **Text is untouched.** `web/demo.epub` (2034 pages): blue-noise vs Floyd–Steinberg differ on
  74 pages — exactly the pages carrying images — and 0.03% of bits. Old build vs new at 1-bit:
  the same 74 pages. The control cannot move text.
* **Pagination is untouched.** Image size does not depend on dithering: page counts are
  identical across all 10 models at both depths (14 pages on `demo-images.epub`, 2034 on the
  novel).
* **Every model is deterministic** (two runs byte-identical), including Zhou–Fang: the reference
  implementation modulates thresholds with `Math.random()`, which would make exports
  irreproducible, so the engine feeds the same formula a deterministic integer hash instead.
  That is a deliberate deviation, and the only one.
* **wasm == native.** The browser's export of `demo-images.epub` with defaults: 1,345,804 B,
  14 pages, payload sha `52672f84f2f7` — identical to the host binary's (same size, pages, sha).

## Where it lives

| piece | change |
|---|---|
| `vendor-lib/Epub/Epub/converters/DitherUtils.h` | `ZHOU_FANG` mode + its tables (ported from the converter, `ZF_MOD_KEYS`/`ZF_COEFF_KEYS`); `ditherNoise01()` deterministic noise; `ErrorDiffusionDither::scatter()` extracted so the 2-tone path shares the kernels; `applyTwoTone()`, `applyZhouFang()` |
| `vendor-lib/Epub/Epub/converters/ImageDither.h` | `ImageDitherOptions` (mode + tone depth + profile) and `ImageDitherer`, one instance per decoded image, called per pixel in scan order; owns the error rows |
| `JpegToFramebufferConverter.cpp`, `PngToFramebufferConverter.cpp` | all 6 dither call sites route through the ditherer; context gains a `ko::ImageDitherer` |
| `src/ko_engine_driver.h` | `Spec::imageDither`, `Spec::imageToneDepth`; `applyImageDitherOptions()` at the top of `renderPage` |
| `src/wasm_api.cpp` | `ko_set_image_dither()`, `ko_set_image_tone_depth()` |
| `src/host_main.cpp` | `--image-dither N`, `--image-dither-name NAME`; `--1bit` implies depth 2 |
| `web/index.html`, `web/app.js`, `web/ko.worker.js` | the "Image dither" select, forwarded to the engine; the tone depth follows the export mode radio |

Coordinates: ordered models keep absolute screen coordinates (as before, so the blue-noise tile
phase does not move), while the error-diffusion buffers use image-local columns — an inset image
would otherwise run off the end of the error rows.

## Verify it

```
scripts/verify/dither_model_sweep.py <host> <book>     # every model, both depths: distinct? deterministic?
scripts/verify/plane_compare.py <ref.xtch> <cand.xtc> --list 7   # tone/ink/pepper in reflectance space
build/ko_xtch_host <book.epub> out.xtc --1bit --image-dither 3   # Floyd-Steinberg, 2 tones
```
