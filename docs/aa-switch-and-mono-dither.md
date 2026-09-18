# The anti-aliasing switch: firmware text AA in 2-bit, blue-noise dither in 1-bit

2026-09-18. Code: `src/xtch_writer.h` (dither), `src/ko_engine_driver.h` (AA gating),
`src/wasm_api.cpp` (`monoDitherEnabled`, 1-bit preview), `web/app.js` + `web/index.html` (the switch).

## 1. What each position means

| output | AA **on** | AA **off** |
|---|---|---|
| 2-bit XTCH (XTH) | text anti-aliased: the grey (lsb/msb) planes carry the 2-bit glyph coverage, exactly as the firmware does it | text renders 1-bit (the grey passes never draw text; only images reach the grey planes) |
| 1-bit XTC (XTG) | grey levels are **blue-noise halftoned** into the 1-bit plane: pseudo-grey text and photos | greys dropped to a **hard threshold** — no dither (crisper text, harsher photos) |

The 2-bit behaviour is the pre-existing firmware-mirroring semantics and is unchanged. The
1-bit behaviour is new: the blue-noise dither used to be an unconditional default
(`monoGrayDither_ = true`), switched only by a hidden host flag. It is now driven by the same
switch the UI exposes, in both the writer and the preview.

## 2. Why the engine had to change for AA off

Under the text-once path the page is rendered once (BW pass) and the grey planes are composed
from a per-pixel level capture, so `textOnce` alone decided whether text greys existed — which
silently made AA a no-op for 2-bit output. The capture is now gated on the AA switch as well:

```cpp
const bool captureText = textOnce && aaOn;   // ko_engine_driver.h
```

so AA off still means "no text greys" (that is what "AA off" is) while AA on keeps the
single-blit speedup. Gated: with AA off, 0/2,034 pages differ from the pre-stage-A engine; with
AA on, 0/2,034 as before.

## 3. Preview == file (the invariant the site promises)

A dithered 1-bit page is not a function of the BW plane, so the 1-bit preview could no longer be
"the BW plane, as-is": it would show un-dithered pages while the file carried halftoned ones.
`ko_compose_rgba(1)` now applies **the same blue-noise masks** the writer inks with
(`ko::monoNoiseMasks()`), so the preview shows the page the file will carry:

* dither on: `ink = ~bw_bit`, then kept only where the mask for that pixel's grey level allows —
  layer 2 (black, 255), layer 0 (dark grey v=1, 235), layer 1 (light grey v=2, 170);
* dither off: the BW plane verbatim (hard threshold).

Both `ko_export_set_mode`/`ko_export_spine` and `ko_compose_rgba` read one helper,
`monoDitherEnabled()` = `spec.textAntiAliasing != 0`, so the two cannot drift apart.

Verified with `scripts/verify/mono_preview_vs_file.js`: for a given spine/page it composes the
preview the worker would blit and compares it, pixel for pixel, against the page inside the real
exported XTG file, in **both** switch positions. Green on the image book (spine 1) and on the text
book (cover, spine 3 p2, spine 10 p0, spine 25 p1, spine 59 p0) — e.g. the cover matches at
112,389 ink px with AA on and 149,626 with AA off, 0 mismatches of 384,000 in each case.

### The pitfall that cost a debugging round (read before touching the compose)

A plane byte holds logical row `y`'s pixels at bit `7 - (y & 7)`, while a **mask** byte holds
column `x`'s pixel at bit `7 - (x & 7)` — the two bit positions are different. `addMonoPage` gets
away with a single byte-wise `inkBits & maskByte` only because it first **transposes** the eight
strided plane bytes into the mask's layout, so in the writer both operands agree per bit. A
per-pixel compose must therefore select the mask bit per pixel:

```cpp
const int l = (lsbByte >> (7 - b)) & 1;      // b = y & 7  -> plane layout
const int m = (msbByte >> (7 - b)) & 1;
const uint8_t maskByte = nm.m[l ? 0 : (m ? 1 : 2)][y & 63][x >> 3];
ink = (maskByte >> (7 - (x & 7))) & 1;       // mask layout
```

ANDing `~lsb & msb` with the mask byte directly (the obvious mirror of the writer) compares
unrelated pixels' bits and silently produces a plausible-looking wrong page: no ink added or
removed wholesale, just ~7% of grey pixels decided by the wrong noise sample. Symptom in the
verifier: preview ink ≠ file ink by a few hundred pixels, every mismatch confined to grey areas.

## 4. Gate results (2,034-page Korean text book + 14-page image book)

| check | result |
|---|---|
| 2-bit XTCH, AA on: new engine vs pre-stage-A reference | **0/2034 differ** |
| 2-bit XTCH, AA off: new engine vs pre-stage-A reference | **0/2034 differ** |
| 1-bit XTG, AA on: dither (follows AA) vs dither forced off | 2034/2034 differ — the switch is live |
| 1-bit XTG, AA off: dither off (follows AA) vs forced on | 2033/2034 differ — thinning only |
| grey content of the 2-bit file with AA off | 74/2034 pages carry any grey, and only image pages (page 0/6/83 counts are byte-identical to AA on) |
| 1-bit preview vs exported file, AA on and off | pixel-exact (`scripts/verify/mono_preview_vs_file.js`) |

Host flags for this: `--1bit`, `--text-aa` / `--no-text-aa`, `--mono-dither` /
`--no-mono-dither` (the last two force the flag to test the AA relationship; without them the
dither follows AA).

## 5. Known nuance of the dither (pre-existing, not introduced here)

The masks are built as `noise < density` with densities {235, 170, 255} and the table holds all
256 values, so 1 in 256 *black* pixels is deliberately not inked — that is the "255 layer" noted
in `xtch_writer.h`. It is why AA-off + dither-on still differs from AA-off + dither-off on pages
with no greys at all (thinning of solid ink), and why the two are not byte-identical even where
the grey planes are empty. AA off is the no-thinning mode.
