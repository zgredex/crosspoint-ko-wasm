# Does 1-bit blue-noise dithering earn its keep? (measured vs non-dithered, scored against 2-bit + AA)

Question: in 1-bit (XTC) output, is halftoning the greys with the blue-noise masks better than
not dithering — measured against the 2-bit + AA page as the reference?

Short answer: **yes, and the reason is specific — it is the only candidate that reproduces the
panel's grey *reflectance*.** A threshold cannot: it can only make a grey pixel black or white,
so every grey level it touches has a systematic tone error. Dithering's obvious cost is real
and measurable: far more isolated dots, and a worse per-pixel error by construction.

Harness: `scripts/verify/dither_decision.js <book> <spine> <pages...> [--aa 0|1]`.
Everything is taken from the engine — `ko_compose_rgba(0)` for the 2-bit+AA reference,
`ko_compose_rgba(1)` for the shipped 1-bit path — so no candidate is simulated.

## Scoring space matters more than the metric (the trap this work hit)

First pass scored against the **preview's** greys (255/128/205/0 from `kGray32`) and concluded
the opposite: the shipped halftone looked like it laid down 92% ink on a level worth "50%", so
it was judged 3-7x worse on tone than a plain threshold. That was the metric's fault. The
per-level ink densities (92.2% dark grey, 67.5% light grey) matched `kMonoInkDensity`
exactly, which forced the question of what those greys *mean*:

```
// src/xtch_writer.h
// panel's four states perceived as white 210 / light grey 80 / dark grey 30 / black 15
// density = (210 - R) / (210 - 15)   ->  {0, 235, 170, 255}
```

`{15, 30, 80, 210}` is the project's panel model, the same anchors the image converter's
4-level thresholds use (`kProfileMaster`, `DitherUtils.h`). So the correct scoring space is
**reflectance**: reference per level = {210, 30, 80, 15}, a 1-bit candidate = {210 white, 15 ink},
and the ideal ink density per level = {(210-R)/195} = {0, 92.3%, 66.7%, 100%}. The preview's RGB
greys are a *display* approximation and scoring against them silently rewrites the target as
"dark grey = 50% ink". All numbers below are reflectance-space, in % of the 195-unit range.

## Results

`pxErr` = mean |reflectance difference| per pixel (/195). `toneErr` = |local (5x5) mean
reflectance difference|. `ink` = % of that level's pixels inked vs its ideal density.

### Text pages — Korean novel, spines 6 and 9, 3 pages each (ref ink 14.1%, greys 5.6%)

| candidate | pxErr | toneErr | dark grey ink | light grey ink | black ink | black lost | pepper/1k |
|---|---|---|---|---|---|---|---|
| **blue-noise (shipped)** | 2.41 | **0.54%** | 91.54 (ideal 92.3) | 66.28 (66.7) | 99.67 (100) | 0.33% | 0.19 |
| no dither: 50% rule | 3.01 | 1.36% | 100 (92.3) | **0.00** (66.7) | 100 | 0% | 0.04 |
| no dither: raw BW | **1.65** | 0.84% | 100 (92.3) | 100 (66.7) | 100 | 0% | 0.01 |

### Grey-heavy page — `demo-images.epub` spine 5 p1 (ref ink 21.9%, **greys 15.3%**)

| candidate | pxErr | toneErr | dark grey ink | light grey ink | black ink | black lost | pepper/1k |
|---|---|---|---|---|---|---|---|
| **blue-noise (shipped)** | 12.10 | **2.18%** | 92.24 (92.3) | 67.48 (66.7) | 99.58 (100) | 0.42% | 65.75 |
| no dither: 50% rule | 17.67 | 8.83% | 100 (92.3) | **0.00** (66.7) | 100 | 0% | 1.59 |
| no dither: raw BW | **8.98** | 4.60% | 100 (92.3) | 100 (66.7) | 100 | 0% | 93.37 |

### Lighter image pages — `demo-images.epub` spine 9 (2 pages, ref ink 8.8%)

| candidate | pxErr | toneErr | dark grey ink | light grey ink | pepper/1k |
|---|---|---|---|---|---|
| **blue-noise** | 1.58 | **0.41%** | 91.83 (92.3) | 66.74 (66.7) | 0.25 |
| no dither: 50% rule | 1.82 | 0.77% | 100 | 0.00 (66.7) | 0.01 |
| no dither: raw BW | **1.04** | 0.54% | 100 | 100 (66.7) | 0.00 |

## Verdict

1. **Blue-noise wins tone on every page tested**, and by the widest margin where greys matter
   most: 2.18% vs 8.83% (50% rule) and 4.60% (raw BW) on the grey-heavy page, 0.54% vs
   1.36%/0.84% on text. Its per-level ink densities land within 0.1-1.1 points of the panel
   model's ideal on every level — i.e. the dithered patch is the level, in average reflectance.
2. **A threshold cannot represent a grey at all.** The 50% rule reproduces light grey with 0%
   ink where the panel model wants 66.7% (a 10.6-16.8% tone error at that level); raw BW
   over-inks light grey at 100%. Those are level errors, not noise: they are wrong at reading
   distance, not just up close.
3. **The costs are the textbook dither costs, and they are visible**: pepper rises from
   ~0.01/1k to 0.19/1k on text and 1.6/1k to 65.75/1k on a grey-heavy page (the price of
   rendering 15% of a page as dots), and per-pixel error is higher than the over-inking
   alternatives (12.10 vs 8.98 on the grey page) — dithering trades per-pixel accuracy for local
   tone on purpose. 0.33-0.42% of black pixels do not ink: that is the deliberate 1-in-256 miss
   of the `255` layer (`kMonoInkDensity[3]`), required for firmware parity, not AA thinning.
4. **Compared with 2-bit + AA**: the dithered 1-bit page reproduces the 4-level page's local
   tone to within ~2% of the reflectance range on the hardest page measured, and exactly
   matches its grey reflectance on average — as a binary stand-in for a 4-level page that is
   about as good as it gets. 2-bit + AA is still strictly better (it *has* the four levels, no
   dot texture at all) and costs 96,022 B/page vs 48,022 B/page — exactly 2x on the payload —
   which is the real trade: half the bytes for dot texture instead of greys.
5. **Consequence for the AA switch in 1-bit**: it does not touch grey halftoning (that always
   happens — it is the whole mechanism above); it only removes the 1/256 solid-ink thinning.
   With AA off the reference loses its greys too, and every candidate then matches it exactly
   (measured: 0.00 error for all three), i.e. "AA off" in 1-bit means "no greys to dither".

Nothing here argues for changing the shipped 1-bit behaviour. The one thing this measurement
does argue against is judging dither quality in the preview's RGB space, which is where the
first pass of this very analysis went wrong.

## Addendum: the candidates changed when the models landed (docs/image-dither-models.md)

The measurements above compare ways of deriving 2 tones FROM THE 4-LEVEL INTERMEDIATE, because
that is what the engine did at the time. The model work replaced that two-stage route with one
pass from the SOURCE image, which changes the candidate set: "no dither" now thresholds the
source at the panel midpoint instead of thresholding a 4-level page, and the two-stage route is
gone.

Re-measured on the same page (`demo-images.epub` spine 5 p1), against the 4-level page, from the
exported planes:

| 1-bit route | tone error | ink | pepper/1k |
|---|---|---|---|
| two-stage (4-level dither -> writer masks) - the OLD behaviour | 8.58% | 17.33% | 65.7 |
| single pass, blue noise | **0.68%** | 8.16% | 5.1 |
| single pass, hard threshold | 1.26% | 7.72% | 0.9 |

The old route's 8.58% was the `kofork`-levels vs panel-model mismatch (greys declared as nominal
0/85/170/255 while the halftone masks assume the panel's 15/30/80/210), which over-inked the
page's greys ~2x. With one pass from the source, the ranking is unchanged in kind - dithering
wins tone, a threshold is the cleanest - but the margins are smaller, because thresholding the
source is a much better decision than thresholding an already-dithered intermediate.
