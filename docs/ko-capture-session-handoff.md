# Single-text-blit path (stage "A") — session handoff

**Status: NOT complete. Implementation is done and measured; byte-exactness is 3 pages short.**

Paste this file's path into a fresh session and it has everything needed to finish A without
re-deriving anything. Written 2026-09-17 at the end of a context-exhausted session.

---

## 1. What A is

`src/ko_engine_driver.h::renderPage` renders every page **three times** — once per plane
(BW, GRAYSCALE_LSB, GRAYSCALE_MSB) — re-walking the whole page layout and re-blitting every glyph
each time. Measured split for the 2,034-page text book:

```
pass1(ink) 38%  |  pass2(lsb) 28%  |  pass3(msb) 34%  |  planes+copy 0.3%
```

The "single-text-blit" path (`const bool textOnce` in `ko_engine_driver.h`) renders the page **once**
in BW mode, capturing each pixel's coverage level, then composes both gray planes from that capture
via `orCapturedGrayInto()` — skipping the two extra layout walks.

## 2. Measured state

```
renderPage : 1,412.6 -> 930.7 ms   (-34%)
total      : 1,559.3 -> 1,078.0 ms
GATE       : 2,031 / 2,034 pages byte-identical
residual   : 86 pixels on 3 pages
             page  415: 48 px  x 41-255   y runs 360-364 / 507-511 / 654-658
             page  416: 32 px  x 69-206   y runs 66-70 / 259-263
             page 2033:  6 px  x 303-407  y 665-666
```

Reference (textOnce OFF, current tree): `/tmp/ab/txt_instr.xtch` (2,034 pages, 195,347,364 B).

## 3. Root causes found (all verified by reading source or by measurement)

1. **`captureLevel()` had ZERO call sites.** `beginLevelCapture()` filled a 192 KB buffer with
   white (3) and nothing ever wrote a level, so the composed planes contained none of the text's
   levels. Divergence was 2,026/2,034 pages.
2. **`captureLevelPhysical` ASSIGNED while the gray passes OR.** `*p = (old & ~m) | (level << sh)`
   vs `fb[idx] |= mask` — so a pixel drawn twice with different levels kept only the last. Fixing
   this to accumulate one bit per plane contribution (bit0 = LSB-trigger v==1, bit1 = MSB-trigger
   v==2), changing the pre-fill `0xFF` → `0x00`, and making `orCapturedGrayInto`'s equality tests
   bit tests took divergence **299 → 3 pages**.
3. **Proof the capture machinery was never finished:** `captureLevelPhysical` and
   `orCapturedGrayInto` existed but nothing called the former.

## 4. Engine model (important — this was a wrong assumption for most of the session)

`GfxRenderer::drawPixel` (GfxRenderer.cpp:603) is **completely mode-agnostic**:

```cpp
if (state) { target[byteIndex] &= ~(1 << bitPosition); }  // state=true  CLEARS (ink)
else       { target[byteIndex] |= 1 << bitPosition; }     // state=false SETS
```

The gray planes are **not** separate render targets — they are the framebuffer copied out after a
mode-specific render:

```cpp
GfxRenderer.cpp:2188  copyGrayscaleLsbBuffers() { display.copyGrayscaleLsbBuffers(frameBuffer); }
GfxRenderer.cpp:2190  copyGrayscaleMsbBuffers() { display.copyGrayscaleMsbBuffers(frameBuffer); }
```

The modes control **which blits draw** (BW: `v < 3` includes black; MSB: `v == 1 || v == 2`;
LSB: `v == 1`), not what a bit means. So a plane bit means *"a draw occurred at this pixel in this
mode"* — which is what the capture's bit encoding models. This is also why the notes' claim
"the ink plane diverges too" was a **tooling artefact**: `plane_split.py` reports *derived* v, so
missing gray levels change the ink count while the BW plane is untouched.

## 5. Tooling bug that corrupted the last three measurements — READ THIS

`/tmp/ab/apply_capture2.py` **accumulates `INS.append(...)` entries across runs.** Each
`finish_a*.py` appends to it and nothing is ever removed. Consequence: the three runs that all
reported **618 differing pages with byte-identical bits** (page 34, plane1=28, plane2=0) were the
**same contamination** — the `drawPixel` choke point (edit #13) was in the file for all of them.

Corrected attribution:

| experiment | result | valid? |
|---|---|---|
| `drawPixel` choke point (unrecorded-pixels-only capture) | 618 pages | **VALID** — caused it (3 → 618) |
| 1-bit glyph branch capture at level 1 | 618 pages | **INVALID** — choke point still in file |
| dither template captures | 618 pages | **INVALID** — choke point still in file |

**Do not trust any measurement from `apply_capture2.py` until it is rebuilt clean.**

## 6. The residual's best-supported cause (identified, NOT verified)

The `drawPixelDither` templates (GfxRenderer.cpp:1000-1021) draw **checkerboards**:

```cpp
1006  drawPixelDither<Color::Black>     -> drawPixel(x, y, true);
1011  drawPixelDither<Color::White>     -> drawPixel(x, y, false);
1016  drawPixelDither<Color::LightGray> -> drawPixel(x, y, x % 2 == 0 && y % 2 == 0);
1021  drawPixelDither<Color::DarkGray>  -> drawPixel(x, y, (x + y) % 2 == 0);
```

In the LSB/MSB passes these set a **regular 2x2 checkerboard of update bits** — matching the
residual exactly: thin 1px differences in a regular grid (which is how the notes have described it
since the beginning), wide x spans, few rows, both planes. A dithered fill region (shaded panel or
header) is the expected source.

## 7. Complete drawPixel call-site inventory (from `GfxRenderer.cpp`)

```
renderCharScaled : 335 (2-bit, PATCHED)   357 (1-bit, NOT patched)
renderCharImpl   : 535/538/540 (BW, PATCHED)  547/550/552 (MSB, PATCHED)  557/560/562 (LSB, PATCHED)
                   586/589/591 (1-bit branch, no renderMode dispatch, NOT patched)
drawLine         : 830/837/850   (NOT patched)
dither templates : 1006/1011/1016/1021 (NOT patched)
maskRoundedRectOutsideCorners : 1228-1231 (NOT patched)
drawIcon         : 1397 (NOT patched)
drawBitmap       : 1498/1500/1502 (PATCHED)
drawBitmap1Bit   : 1570 (PATCHED)
fillPolygon      : 1632 (NOT patched)
```

## 8. The correct patch set (to rebuild clean)

Keep these (all verified to bring the state to **3 pages**):

1. `renderCharScaled` (portrait fast path) — capture in all three `renderMode` branches **and** the
   `boldOk` twin, using `captureLevelPhysical(phyX, phyY0 - gx, v)` and `(phyX, phyY0 - gx - 1, v)`.
2. `renderCharScaled` 2-bit `maxRaw` block — capture `bmpVal`-equivalent level (this one is a
   *derived* rule and is suspect).
3. `renderCharImpl` MSB + LSB branches + bold twins — `captureLevel(screenX, screenY, bmpVal)`.
4. `renderCharImpl` BW branch — `captureLevel(screenX, screenY, bmpVal)`.
5. `drawBitmap` MSB + LSB branches — `captureLevel(screenX, screenY, val)`.
6. `drawBitmap1Bit` 1569 — `captureLevel(screenX, screenY, 1)`.
7. **Semantics fixes:** pre-fill `0x00`; `captureLevelPhysical` accumulates
   (`level==1 -> bit0`, `level==2 -> bit1`, nothing for 0/3); `orCapturedGrayInto` bit tests
   (`lv & (lsbPlane ? 0x1u : 0x3u)`).

Exclude: the `drawPixel` choke point (proven harmful, 618 pages) and the 1-bit branch captures
(unproven — retest cleanly).

Then, **on that clean base only**, add the dither captures (item 6 in §6: mirror each template's own
condition, capturing only where the expression yields *false*, i.e. the setting case) and gate.

Level choice per template to test in order: `LightGray -> 2`, `DarkGray -> 1`, `White -> 2`, then
`White -> 1` if the gate fails.

## 9. Gate protocol (non-negotiable)

Only meaningful with the path **ENABLED** (`textOnce = true`): with it disabled `_levelCapture` is
false, `captureLevel` returns immediately, and the gate passes regardless of how wrong the capture
is. There is no useful "capture inert" step.

Page-level comparison, never whole-file `cmp` (the container carries a varying millis-derived field).
XTH page payload = 96,000 B (`p1` = light level, `p2` = dark level); XTH records are 96,022 B with a
22-byte header. `plane_split.py` parses XTH; XTG (mono) needs its own parser.

Apply → build → gate → **auto-revert on failure**. Every experiment in this session used that shape
and left the tree verified-clean.

## 10. Related banked work (committed, verified)

- Mono writer: **590.3 → 231.8 ms (2.54x)** via bit-parallel output bytes with a three-layer noise
  mask and an 8x8 bit transpose (self-tested exhaustively against a naive reference). Committed.
- `-msimd128` + `-flto` are in the wasm build but emit **zero** SIMD — measured, and wasm SIMD has
  no gather instruction, so the mono writer's strided gather is not addressable by SIMD.
- Branchless compose: 1.498 → 0.219 ms/page (6.8x), 17x cumulative from the original JS.
- `build_and_deploy.sh` needs a retry loop on its live-hash check (propagation latency causes false
  FAILs).

## 11. Environment gotchas

- `env -i PATH=/opt/homebrew/bin:/usr/bin:/bin` — plain `env -i` strips PATH.
- No `timeout`; use a script file rather than `python -c` for anything containing quotes/escapes
  (two failures this session came from escape mangling in generated patches).
- `grep -c` exits 1 on zero matches, breaking `&&` chains.
- Foreground terminal timeout max 600 s.
- Use `repr()`-built patch scripts, not embedded escapes.
