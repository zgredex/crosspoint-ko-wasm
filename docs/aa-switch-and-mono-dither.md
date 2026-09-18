# Image dithering and the anti-aliasing switch

2026-09-18. Code: `vendor-lib/Epub/Epub/converters/DitherUtils.h` (4-level image
quantiser), `src/xtch_writer.h` (4-level → 2-level halftone), `src/ko_engine_driver.h` (AA
gating), `src/wasm_api.cpp` + `web/app.js` + `web/index.html` (the switch).

## 1. Images are dithered in EVERY mode. The AA switch is about text.

| output | images | what the AA switch changes |
|---|---|---|
| 2-bit XTCH (XTH) | **always** blue-noise dithered to the 4 panel levels | whether the grey planes carry *text* greys (AA on) or text renders 1-bit (off) |
| 1-bit XTC (XTG) | **always** blue-noise halftoned to 2 levels (ink / no ink) | AA on also halftones text greys and thins solid ink; AA off leaves text as crisp 1-bit ink |

Both dither stages use the same 64×64 `kBlueNoise64` void-and-cluster table, but at
different points in the pipeline:

```
JPEG/PNG decode ──(applyOrderedDither4Level: blue noise, 4 levels)──▶ 2-bit planes
2-bit planes ──(mono writer: blue-noise masks at 235/170/255)──▶ 1-bit XTG ink
```

## 2. Stage 1: 8-bit grey → 4 levels (`DitherUtils.h`, decoders)

`ImageBlock` sets `config.useDithering = true` (default in `ImageToFramebufferDecoder.h` too),
and both converters call `applyOrderedDither4Level(gray, x, y)`, which is
`applyOrderedDither(..., DitherMode::BLUE_NOISE, kDefaultProfile)` = the `kofork` profile
(levels {0, 85, 170, 255}; hard thresholds {45, 70, 140} used only when dithering is off).

Unit-tested with constant-grey 64×64 patches (`DitherUtils.h` still carries the pre-change
`applyBayerDither4Level` as the A/B anchor, which is the natural positive control):

| grey | blue noise: tone | ac lag 1 | ac lag 4 | Bayer 4x4: tone | ac lag 4 |
|---|---|---|---|---|---|
| 20 | 7.8% | −0.25 | −0.03 | 0.0% | +0.00 |
| 60 | 23.6% | −0.24 | −0.04 | 14.6% | **+1.00** |
| 110 | 43.1% | −0.26 | −0.02 | 41.7% | **+1.00** |
| 170 | 66.7% | +0.00 | +0.00 | 72.9% | **+1.00** |
| 220 | 86.3% | −0.26 | −0.04 | 93.8% | **+1.00** |
| 246 | 96.5% | −0.12 | +0.00 | 100.0% | +0.00 |

"ac lag 4" is the self-correlation of the pattern at lag 4: **+1.000 for Bayer means the
pattern is exactly periodic with period 4** — the visible cross-hatch that was replaced. The
blue-noise field is aperiodic and mildly anti-correlated at short range, and follows the input
tone over the whole range. With dithering off the hard triples (45/70/140) collapse everything
above grey 140 to black, which is why dithering is not optional for photos.

**"ac lag 4" is the discriminator to reuse**: any future change to the image dither should keep
this near zero and keep the tone column tracking the grey.

## 3. Stage 2: 4 levels → 1 bit (the mono writer)

`addMonoPage` inks a pixel by comparing the blue-noise field against the density of its 4-level
value: layer 0 (dark grey, v=1) 235/255, layer 1 (light grey, v=2) 170/255, layer 2 (black,
v=3) 255/255; white never inks. Grey pixels are masked **unconditionally** — that is the
4→2-level halftone that images depend on — while solid ink (no grey at all) is thinned by the
255 layer **only with text AA on**.

Measured realised ink density per 4-level value, on both books (`/tmp/ab/image_dither_report.py`
in the session; the invariant to re-check):

| 4-level value | expected | text book, AA on | text book, AA off | image book, AA off |
|---|---|---|---|---|
| white (v=0) | 0.000 | 0.000 | 0.000 | 0.000 |
| dark grey (v=1) | 0.922 | 0.918–0.920 | 0.920 | 0.923 |
| light grey (v=2) | 0.667 | 0.664–0.668 | 0.667 | 0.656 |
| black (v=3) | 1.000 | 0.996 (thinned) | **1.000** (crisp) | **1.000** |

So with AA off in 1-bit mode, text pages contain **no greys at all** (dark/light counts are
literally 0) and every black pixel is ink — crisp text — while image greys still halftone at the
correct densities. With AA on, text greys are halftoned like everything else and solid ink is
thinned by 1/256 (the `noise < 255` layer noted in `xtch_writer.h`).

## 4. What the switch is, precisely

* Engine: `captureText = textOnce && aaOn` (`ko_engine_driver.h`) — with AA off the grey passes
  render images only, so a page carries no text greys. Gated: 0/2,034 pages differ from the
  pre-stage-A engine with AA off, and 0/2,034 with AA on.
* Writer + preview: one helper, `textAaEnabled() = spec.textAntiAliasing != 0`, read by
  `ko_export_set_mode`, re-asserted in `ko_export_spine`, and by `ko_compose_rgba` — the
  preview shows the page the file will carry in either position.
* Host flags: `--1bit`, `--text-aa` / `--no-text-aa`. (The old `--mono-dither` /
  `--no-mono-dither` are gone: there is no separate dither switch to force any more.)

## 5. Gates run

| check | result |
|---|---|
| 2-bit, AA on: vs pristine reference | 0/2034 pages differ |
| 2-bit, AA off: vs pre-stage-A engine | 0/2034 pages differ |
| 1-bit, AA on: vs the previously deployed build | **byte-identical** (0/2034) |
| 1-bit, AA off: image greys still halftoned, text crisp | densities table above |
| 1-bit preview vs exported file, both switch positions | pixel-exact on 4 spine/page cases |
| image quantiser: 4-level dither quality vs the Bayer anchor | unit test, §2 |
